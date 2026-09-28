#include <gtest/gtest.h>
#include "headless/headless_session_scope.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/thread_service.hpp"
#include "session/scoped_subscription.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include <memory>
#include <stdexcept>

namespace {
using namespace acecode;

// 场景与期望：headless 提交失败早退仍退订并清回填指针，避免订阅或依赖逃出局部作用域。
TEST(HeadlessSessionScope, RejectedSubmissionUnsubscribesAndClearsBackfillBeforeServiceShutdown) {
    acecode_test::characterization::Isolation isolation;
    AppConfig config;
    config.session_title.enabled = false;
    ToolExecutor tools;
    PermissionManager permissions;
    auto subagents = std::make_shared<SubagentToolDeps>();
    auto threads = std::make_shared<ThreadToolDeps>();
    std::shared_ptr<SessionEntry> retained;
    std::shared_ptr<SessionEntry> other_session;
    auto deliveries = std::make_shared<int>(0);
    const int exit_code = [&] {
        SessionRegistryDeps deps;
        deps.cwd = path_to_utf8(isolation.directory.path);
        deps.tools = &tools;
        deps.config = &config;
        deps.template_permissions = &permissions;
        SessionRegistry registry(std::move(deps));
        LocalSessionClient client(registry);
        auto cleanup = headless::make_session_cleanup(registry, subagents, threads);
        subagents->registry = &registry;
        subagents->client = &client;
        subagents->config = &config;
        threads->service = std::make_shared<ThreadService>(ThreadService::Deps{&registry, &client});
        const auto id = registry.create({});
        retained = registry.acquire(id);
        other_session = registry.acquire(registry.create({}));
        // Same declaration order as the runner: wait state then subscription.
        bool wait_state_alive = true;
        ScopeExit destroy_wait_state([&] { wait_state_alive = false; });
        ScopedSubscription subscription(client, id, client.subscribe(id,
            [deliveries, &wait_state_alive](const SessionEvent& event) {
                EXPECT_TRUE(wait_state_alive);
                if (event.kind == SessionEventKind::Token) ++*deliveries;
            }));
        // A session removed between subscribe and submit is a real false path
        // through LocalSessionClient::send_input, with the old entry retained.
        registry.destroy(id);
        if (!client.send_input(id, "rejected")) return 1;
        return 0;
    }();
    EXPECT_EQ(exit_code, 1);
    EXPECT_EQ(subagents->registry, nullptr);
    EXPECT_EQ(subagents->client, nullptr);
    EXPECT_EQ(subagents->config, nullptr);
    EXPECT_EQ(threads->service, nullptr);
    EXPECT_TRUE(other_session->sm->current_session_id().empty());
    EXPECT_FALSE(other_session->loop->is_busy());
    retained->loop->events().emit(SessionEventKind::Token, {{"text", "after early return"}});
    EXPECT_EQ(*deliveries, 0);
    // This point is where run_print_mode closes MCP/LSP, outside its IIFE.
}

// 场景与期望：装配中途异常也须收回已建立的回填引用，防止后续工具访问已析构 registry。
TEST(HeadlessSessionScope, SetupExceptionAlsoClearsPartialBackfill) {
    acecode_test::characterization::Isolation isolation;
    ToolExecutor tools;
    SessionRegistryDeps deps;
    deps.cwd = path_to_utf8(isolation.directory.path);
    deps.tools = &tools;
    SessionRegistry registry(std::move(deps));
    LocalSessionClient client(registry);
    auto subagents = std::make_shared<SubagentToolDeps>();
    auto threads = std::make_shared<ThreadToolDeps>();
    EXPECT_THROW({
        auto cleanup = headless::make_session_cleanup(registry, subagents, threads);
        subagents->registry = &registry;
        subagents->client = &client;
        throw std::runtime_error("injected setup failure");
    }, std::runtime_error);
    EXPECT_EQ(subagents->registry, nullptr);
    EXPECT_EQ(subagents->client, nullptr);
    EXPECT_THROW(registry.create({}), std::runtime_error);
}
} // namespace
