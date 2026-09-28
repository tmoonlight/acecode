#include <gtest/gtest.h>
#include "tui/render/frame_renderer.hpp"
#include "tui/render/prepare_frame.hpp"
#include "tui/render/tool_row_view.hpp"
#include "tui/render/overlay_views.hpp"
#include "tui/chat/message_render_revision.hpp"
#include "test_support/tui/fake_screen_port.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace {
std::string plain_snapshot(const ftxui::Element& element, int width, int height) {
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, element);
    std::string result;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) result += screen.PixelAt(x, y).character;
        result += '\n';
    }
    return result;
}
}

// 中文回归说明：整帧仍支持 conhost、窄屏和带侧栏的三种布局，编辑器只在常规分支构建。
TEST(TuiFrameRenderer, ThreeRootLayoutsKeepTranscriptAndComposer) {
    for (const auto mode : {std::pair<int, bool>{80, true}, {80, false}, {160, false}}) {
        acecode::TuiState state;
        state.conversation.push_back({"user", "visible-user-message", false});
        acecode::tui::ChatViewport viewport;
        viewport.chat_box = {0, mode.first - 10, 0, 14};
        acecode::tui::FrameGeometry geometry;
        acecode::tui::test_support::FakeScreenPort screen;
        screen.set_width(mode.first);
        std::atomic<int> tick{0};
        acecode::PermissionManager permissions;
        const std::string version = "frame-version", cwd = "C:/frame-workspace";
        int composer_calls = 0;
        auto composer = ftxui::Renderer([&] {
            ++composer_calls;
            return ftxui::text("visible-composer");
        });
        acecode::tui::TuiFrameRenderer renderer(state, screen, version, cwd,
            viewport, geometry, tick, composer, permissions, false, mode.second, false,
            [width = mode.first] { return acecode::tui::FrameTerminalSize{width, 35}; });
        const auto text = plain_snapshot(renderer.render(), mode.first, 35);
        EXPECT_NE(text.find("visible-user-message"), std::string::npos);
        EXPECT_NE(text.find("visible-composer"), std::string::npos);
        EXPECT_NE(text.find("mode:"), std::string::npos);
        EXPECT_EQ(composer_calls, 1);
        state.confirm_pending = true;
        state.confirm_tool_name = "file_read";
        (void)renderer.render();
        EXPECT_EQ(composer_calls, 1);
        EXPECT_TRUE(geometry.input_hit_layout.box.IsEmpty());
        EXPECT_TRUE(geometry.input_hit_layout.regions.empty());
    }
}

// 中文回归说明：清空上一帧命中框之前补偿选区；未测量或焦点改变时不能凭空平移。
TEST(TuiFrameRenderer, PrepareCompensatesSelectionBeforeClearingBoxes) {
    acecode::TuiState state;
    state.conversation.push_back({"user", "one", false});
    state.chat_focus_index = state.last_focus_index = 0;
    state.chat_line_offset = state.last_chat_line_offset = 0;
    state.last_focus_box_y = 3;
    acecode::tui::ChatViewport viewport;
    viewport.chat_box = {0, 75, 0, 9};
    acecode::tui::FrameGeometry geometry;
    geometry.message_boxes = {{0, 75, 8, 8}};
    acecode::tui::test_support::FakeScreenPort screen;
    const auto prepared = acecode::tui::prepare_frame_locked(
        state, screen, viewport, geometry, 80, false);
    EXPECT_EQ(prepared.current_message_width, 76);
    ASSERT_EQ(screen.shifts().size(), 1u);
    EXPECT_EQ(screen.shifts().front(), (std::pair<int, int>{0, 5}));
    EXPECT_EQ(state.last_focus_box_y, 8);
    ASSERT_EQ(geometry.message_boxes.size(), 1u);
    EXPECT_EQ(geometry.message_boxes[0].y_min, 0);
    (void)acecode::tui::prepare_frame_locked(state, screen, viewport, geometry, 80, false);
    EXPECT_EQ(screen.shifts().size(), 1u);
}

// 中文回归说明：失败摘要保留前三行输出；展开后走完整输出，不能继续截成摘要。
TEST(TuiFrameRenderer, ToolResultSummaryAndExpansionKeepOriginalPriority) {
    acecode::TuiState::Message msg;
    msg.role = "tool_result";
    msg.content = "first\nsecond\nthird\nfourth";
    acecode::ToolSummary summary;
    summary.verb = "Run";
    summary.object = "test-command";
    summary.metrics.emplace_back("exit", "1");
    msg.summary = summary;
    const ftxui::Box box{0, 79, 0, 15};
    auto collapsed = plain_snapshot(acecode::tui::render_tool_result_row(
        msg, box, false, false), 80, 10);
    EXPECT_NE(collapsed.find("test-command"), std::string::npos);
    EXPECT_NE(collapsed.find("third"), std::string::npos);
    EXPECT_EQ(collapsed.find("fourth"), std::string::npos);
    auto expanded = plain_snapshot(acecode::tui::render_tool_result_row(
        msg, box, true, false), 80, 10);
    EXPECT_NE(expanded.find("fourth"), std::string::npos);
    EXPECT_EQ(expanded.find("test-command"), std::string::npos);
}

// 中文回归说明：内容变化只失效 Markdown 缓存，不改变布局版本的原有语义。
TEST(TuiFrameRenderer, ContentParticipatesOnlyInRenderCacheRevision) {
    acecode::TuiState::Message msg{"assistant", "a", false};
    const auto layout = acecode::tui::message_render_revision(msg, false);
    const auto before = acecode::tui::message_render_cache_revision(msg, false, "a");
    msg.content = "b";
    EXPECT_EQ(acecode::tui::message_render_revision(msg, false), layout);
    EXPECT_NE(acecode::tui::message_render_cache_revision(msg, false, "b"), before);
}

// 中文回归说明：确认浮层仍显示子任务来源和原来的键盘帮助，ask 帧在每次构建时重置。
TEST(TuiFrameRenderer, ConfirmationOverlayRetainsOriginAndKeyHelp) {
    acecode::TuiState state;
    state.confirm_pending = true;
    state.confirm_origin_label = "child-1";
    state.confirm_tool_name = "file_read";
    acecode::tui::AskQuestionFrame ask;
    const auto overlay = acecode::tui::render_overlay_views(state, ask, 100, 90, false, 20);
    const auto text = plain_snapshot(overlay.confirm, 100, 15);
    EXPECT_NE(text.find("child-1"), std::string::npos);
    EXPECT_NE(text.find("Enter select"), std::string::npos);
    EXPECT_NE(text.find("Esc deny"), std::string::npos);
}
