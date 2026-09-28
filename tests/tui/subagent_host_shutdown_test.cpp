#include <gtest/gtest.h>
#include "tui/subagent_host.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "tool/ask_user_question_tool.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace {
using namespace acecode;
using namespace std::chrono_literals;

class ChildWaitingProvider : public acecode_test::StubLlmProvider {
public:
    void chat_stream(const std::vector<ChatMessage>&, const std::vector<ToolDef>&,
                     const StreamCallback&, std::atomic<bool>* abort) override {
        started.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while ((!abort || !abort->load()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(2ms);
        cancelled.store(abort && abort->load());
    }
    std::promise<void> started;
    std::atomic<bool> cancelled{false};
};

class SubagentHostShutdown : public testing::Test {
protected:
    SubagentHostShutdown() {
        config.session_title.enabled = false;
        config.agent_loop.question_policy = "ask";
        config.agent_loop.question_policy_explicit = true;
        permissions.set_mode(PermissionMode::Yolo);
        tools.register_tool(create_ask_user_question_tool_async());
    }
    std::unique_ptr<tui::SubagentHost> make_host() {
        tui::SubagentHost::Deps deps;
        deps.registry_deps.cwd = path_to_utf8(isolation.directory.path);
        deps.registry_deps.config = &config;
        deps.registry_deps.tools = &tools;
        deps.registry_deps.template_permissions = &permissions;
        deps.publish_tasks = [calls = published](std::vector<tui::SubagentTaskSnapshot>) {
            calls->fetch_add(1);
        };
        deps.on_permission_request = [calls = permission_events](
            const std::string&, const std::string&, nlohmann::json) { calls->fetch_add(1); };
        return std::make_unique<tui::SubagentHost>(std::move(deps));
    }
    static void install(SessionEntry& entry, std::shared_ptr<LlmProvider> provider) {
        const auto state = entry.model_binding->state_snapshot();
        entry.model_binding->install_runtime_snapshot(
            std::move(provider), state, entry.model_binding->applied_revision());
    }
    acecode_test::characterization::Isolation isolation;
    AppConfig config;
    ToolExecutor tools;
    PermissionManager permissions;
    // Shared by host callbacks and the assertions after host destruction.
    std::shared_ptr<std::atomic<int>> published = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> permission_events = std::make_shared<std::atomic<int>>(0);
};

// 场景与期望：子代理运行中退出 TUI 宿主，应先 join 子会话再释放回调状态，防止析构顺序悬垂。
TEST_F(SubagentHostShutdown, RunningChildStopsBeforeHostCallbackStateIsDestroyed) {
    auto host = make_host();
    const auto id = host->registry().create({});
    auto entry = host->registry().acquire(id);
    auto provider = std::make_shared<ChildWaitingProvider>();
    auto started = provider->started.get_future();
    install(*entry, provider);
    UserInput input;
    input.text = "Wait for host shutdown";
    entry->loop->submit(input);
    ASSERT_EQ(started.wait_for(5s), std::future_status::ready);
    host->on_spawned(id, input.text);
    const auto before = published->load();
    ASSERT_GT(before, 0);
    host.reset();
    EXPECT_TRUE(provider->cancelled.load());
    EXPECT_FALSE(entry->loop->is_busy());
    EXPECT_TRUE(entry->sm->current_session_id().empty());
    EXPECT_EQ(published->load(), before);
    // The entry may outlive its host through a retained client snapshot.
    entry->loop->events().emit(SessionEventKind::BusyChanged, {{"busy", false}});
    entry->loop->events().emit(SessionEventKind::SessionUpdated, {{"title", "late"}});
    entry->loop->events().emit(SessionEventKind::PermissionRequest, {{"request_id", "late"}});
    EXPECT_EQ(published->load(), before);
    EXPECT_EQ(permission_events->load(), 0);
}

// 场景与期望：子代理提问挂起时关停，应取消提问再关闭服务，避免 join 等待永远无法回答的问题。
TEST_F(SubagentHostShutdown, PendingQuestionIsCancelledBeforeServicesCanClose) {
    auto host = make_host();
    const auto id = host->registry().create({});
    auto entry = host->registry().acquire(id);
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("AskUserQuestion", R"({"questions":[{
        "question":"Continue?","header":"Choice","multiSelect":false,
        "options":[{"label":"Yes","description":"continue"},
                   {"label":"No","description":"stop"}]}]})");
    install(*entry, provider);
    auto requested = std::make_shared<std::promise<void>>();
    auto question = requested->get_future();
    auto was_requested = std::make_shared<std::atomic<bool>>(false);
    auto aborted = std::make_shared<std::atomic<bool>>(false);
    ScopedSubscription subscription(entry->loop->events(),
        entry->loop->events().subscribe([requested, was_requested, aborted](const SessionEvent& event) {
            if (event.kind == SessionEventKind::QuestionRequest && !was_requested->exchange(true))
                requested->set_value();
            if (event.kind == SessionEventKind::QuestionClosed)
                aborted->store(event.payload.value("reason", "") == "aborted");
        }));
    UserInput input;
    input.text = "Ask before continuing";
    entry->loop->submit(input);
    ASSERT_EQ(question.wait_for(5s), std::future_status::ready);
    host->on_spawned(id, input.text);
    host->shutdown();
    host->shutdown();
    EXPECT_TRUE(aborted->load());
    EXPECT_EQ(entry->ask_prompter->pending_count(), 0u);
    EXPECT_TRUE(host->running_tasks().empty());
    EXPECT_EQ(host->registry().size(), 0u);
}
} // namespace
