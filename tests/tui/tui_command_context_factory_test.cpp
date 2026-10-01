#include <gtest/gtest.h>
#include "tui/app/tui_command_context_factory.hpp"
#include "tui/model/user_turn_state.hpp"
#include "test_support/tui/input_fixture.hpp"
#include "tool/mcp_manager.hpp"
#include "skills/skill_registry.hpp"
#include "memory/memory_service.hpp"
#include "session_host/memory_runtime.hpp"
#include <filesystem>

namespace {
using acecode::tui::TuiCommandContextFactory;
using acecode::tui::test_support::InputHarness;
struct FakeSurfaces final : acecode::tui::IFullScreenSurfaces {
    std::string selected;
    bool open_settings(const std::string& tab, std::string&) override {
        selected = "settings/" + tab; return true;
    }
    bool open_management(const std::string& tab, std::string&) override {
        selected = "management/" + tab; return true;
    }
};
}

// 中文契约说明：启动/通知上下文没有用量与全屏入口，用户命令上下文包含二者；公共字段完全一致。
TEST(TuiCommandContextFactory, ContextFieldsAndOptionalDirectInputHooks) {
    InputHarness h;
    acecode_test::characterization::Harness agent(h.isolation);
    acecode::SessionModelBinding binding;
    acecode::TokenTracker tracker;
    acecode::McpManager mcp;
    acecode::SkillRegistry skills;
    // 只当成借用指针透传,不触碰磁盘(MemoryService 构造不建目录、不开状态库)。
    acecode::MemoryRuntime memory(std::make_shared<acecode::MemoryService>(
        std::filesystem::temp_directory_path() / "acecode-ctx-memory",
        std::filesystem::temp_directory_path() / "acecode-ctx-memory" / "state.sqlite3",
        acecode::MemoryConfig{}), acecode::MemorySurface::Tui);
    std::unique_ptr<acecode::tui::IFullScreenSurfaces> surfaces = std::make_unique<FakeSurfaces>();
    auto& selected = static_cast<FakeSurfaces&>(*surfaces).selected;
    TuiCommandContextFactory factory(h.state, *agent.loop, binding, h.config, tracker,
        h.permissions, h.screen, h.session, mcp, agent.tools, skills, memory, h.commands,
        h.cwd, h.turn, nullptr, surfaces);
    for (bool direct : {false, true}) {
        auto context = factory.make(direct);
        EXPECT_EQ(&context.state, &h.state);
        EXPECT_EQ(&context.agent_loop, agent.loop.get());
        EXPECT_EQ(context.model_binding, &binding);
        EXPECT_EQ(&context.config, &h.config);
        EXPECT_EQ(&context.token_tracker, &tracker);
        EXPECT_EQ(&context.permissions, &h.permissions);
        EXPECT_EQ(context.session_manager, &h.session);
        EXPECT_EQ(context.mcp_manager, &mcp);
        EXPECT_EQ(context.tools, &agent.tools);
        EXPECT_EQ(context.skills, &skills);
        EXPECT_EQ(context.memory, &memory);
        EXPECT_EQ(context.command_registry, &h.commands);
        EXPECT_EQ(context.cwd, h.cwd);
        EXPECT_EQ(context.subagent_host, nullptr);
        EXPECT_TRUE(context.request_exit);
        EXPECT_TRUE(context.post_event);
        EXPECT_TRUE(context.submit_user_input);
        EXPECT_EQ(bool(context.on_command_recognized), direct);
        EXPECT_EQ(bool(context.open_settings_surface), direct);
        EXPECT_EQ(bool(context.open_management_surface), direct);
        context.post_event();
        EXPECT_EQ(h.screen.take_events(), (std::vector<ftxui::Event>{ftxui::Event::Custom}));
        acecode::UserInput input; input.text = direct ? "direct" : "internal";
        context.submit_user_input(input);
        EXPECT_EQ(h.turn.inputs.back().text, input.text);
        if (direct) {
            std::string error;
            EXPECT_TRUE(context.open_settings_surface("appearance", error));
            EXPECT_EQ(selected, "settings/appearance");
            EXPECT_TRUE(context.open_management_surface("skills", error));
            EXPECT_EQ(selected, "management/skills");
            context.on_command_recognized("help");
            EXPECT_GT(h.state.slash_command_usage_counts["help"], 0u);
        }
    }
}

// 中文生命周期说明：CommandContext 可由 picker 保存，工厂撤销后其中的回调不得访问旧依赖。
TEST(TuiCommandContextFactory, ContextCallbacksAreRevokedWithFactory) {
    InputHarness h;
    acecode_test::characterization::Harness agent(h.isolation);
    acecode::SessionModelBinding binding;
    acecode::TokenTracker tracker;
    acecode::McpManager mcp;
    acecode::SkillRegistry skills;
    // 只当成借用指针透传,不触碰磁盘(MemoryService 构造不建目录、不开状态库)。
    acecode::MemoryRuntime memory(std::make_shared<acecode::MemoryService>(
        std::filesystem::temp_directory_path() / "acecode-ctx-memory",
        std::filesystem::temp_directory_path() / "acecode-ctx-memory" / "state.sqlite3",
        acecode::MemoryConfig{}), acecode::MemorySurface::Tui);
    std::unique_ptr<acecode::tui::IFullScreenSurfaces> surfaces;
    std::optional<acecode::CommandContext> context;
    {
        TuiCommandContextFactory factory(h.state, *agent.loop, binding, h.config, tracker,
            h.permissions, h.screen, h.session, mcp, agent.tools, skills, memory, h.commands,
            h.cwd, h.turn, nullptr, surfaces);
        context.emplace(factory.make(false));
    }
    context->post_event();
    context->request_exit();
    acecode::UserInput input; input.text = "late";
    context->submit_user_input(input);
    EXPECT_TRUE(h.screen.take_events().empty());
    EXPECT_FALSE(h.screen.exited());
    EXPECT_TRUE(h.turn.inputs.empty());
}

// 中文逐字段说明：busy 回调延后写 waiting，Shell 使用固定短语；无关状态不因去重而重置。
TEST(UserTurnState, ResetPreservesBusyOrderAndUnrelatedFields) {
    acecode::TuiState state;
    state.is_waiting = false;
    state.streaming_output_chars = 123;
    state.turn_completion_tokens_confirmed = 456;
    state.turn_interrupted_by_user = true;
    state.chat_follow_tail = false;
    state.input_text = "keep";
    state.tool_running = true;
    state.pending_queue.push_back("queued");
    const auto before = std::chrono::steady_clock::now();
    acecode::tui::begin_user_turn_locked(state, acecode::tui::UserTurnPhrase::Random,
        acecode::tui::WaitingUpdate::Preserve);
    EXPECT_FALSE(state.is_waiting);
    EXPECT_GE(state.thinking_start_time, before);
    EXPECT_EQ(state.streaming_output_chars, 0u);
    EXPECT_EQ(state.turn_completion_tokens_confirmed, 0);
    EXPECT_TRUE(state.turn_interrupted_by_user);
    EXPECT_FALSE(state.chat_follow_tail);
    EXPECT_EQ(state.input_text, "keep");
    EXPECT_TRUE(state.tool_running);
    EXPECT_EQ(state.pending_queue, (std::vector<std::string>{"queued"}));
    acecode::tui::begin_user_turn_locked(state, acecode::tui::UserTurnPhrase::Shell);
    EXPECT_TRUE(state.is_waiting);
    EXPECT_EQ(state.current_thinking_phrase, "Running shell");
}
