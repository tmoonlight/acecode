#include <gtest/gtest.h>
#include "tui/model/initial_state.hpp"
#include "tui/tui_state.hpp"
#include "tool/mcp_manager.hpp"
#include "test_support/agent/stub_provider.hpp"

TEST(TuiInitialState, MissingModelAndQuestionPreferencesSurviveInitialization) {
    // 尚未配置模型也能进入 TUI;问答尺寸和反馈时长来自配置。
    acecode::TuiState state;
    acecode::AppConfig config;
    config.input_history.enabled = false;
    config.tui.question_min_visible_rows = 7;
    config.tui.question_selection_feedback_ms = 180;
    acecode::McpManager mcp;
    acecode::tui::initialize_tui_state_before_screen(state, config, "", false, mcp, {});
    EXPECT_EQ(state.status_line, "No model configured");
    EXPECT_EQ(state.ask_config.min_visible_rows, 7);
    EXPECT_EQ(state.ask_config.selection_feedback_ms, 180);
    EXPECT_TRUE(state.conversation.empty());
}
TEST(TuiInitialState, DangerousStartupWarningIsFirstVisibleMessage) {
    // 危险模式警告在初始对话的原位置出现,不会因模块拆分延迟到首回合后。
    acecode::TuiState state;
    acecode::AppConfig config;
    config.input_history.enabled = false;
    acecode::McpManager mcp;
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    acecode::tui::initialize_tui_state_before_screen(state, config, "", true, mcp, provider);
    EXPECT_EQ(state.status_line, "[" + provider->name() + "] model: " + provider->model());
    ASSERT_EQ(state.conversation.size(), 1u);
    EXPECT_EQ(state.conversation.front().role, "system");
    EXPECT_NE(state.conversation.front().content.find("[DANGEROUS YOLO MODE]"), std::string::npos);
}
