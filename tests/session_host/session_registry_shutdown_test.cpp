#include <gtest/gtest.h>

#include "session_host/session_registry.hpp"
#include "session_host/local_session_client.hpp"
#include "session/scoped_subscription.hpp"
#include "session/session_writer_lease.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "tool/ask_user_question_tool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using namespace acecode;
using namespace std::chrono_literals;

template <typename Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

class SessionRegistryShutdown : public testing::Test {
protected:
    SessionRegistryShutdown() {
        config.session_title.enabled = false;
        config.agent_loop.question_policy = "ask";
        config.agent_loop.question_policy_explicit = true;
        permissions.set_mode(PermissionMode::Yolo);
        tools.register_tool(create_ask_user_question_tool_async());
    }

    std::unique_ptr<SessionRegistry> make_registry(
        std::function<std::optional<std::string>(const std::string&)> generate = {}) {
        SessionRegistryDeps deps;
        deps.cwd = path_to_utf8(isolation.directory.path);
        deps.tools = &tools;
        deps.config = &config;
        deps.template_permissions = &permissions;
        deps.auto_title_generator = std::move(generate);
        return std::make_unique<SessionRegistry>(std::move(deps));
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
};

// Shared only by the event listener and test observer.
struct QuestionEvents {
    std::mutex mu;
    std::condition_variable changed;
    bool requested = false;
    std::string closed;
};

TEST_F(SessionRegistryShutdown, DestructorCancelsPendingQuestionWithRetainedEntry) {
    auto registry = make_registry();
    const auto id = registry->create({});
    auto entry = registry->acquire(id);
    ASSERT_NE(entry, nullptr);
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_tool_call("AskUserQuestion", R"({"questions":[{
        "question":"Continue?","header":"Choice","multiSelect":false,
        "options":[{"label":"Yes","description":"continue"},
                   {"label":"No","description":"stop"}]}]})");
    install(*entry, provider);
    auto observed = std::make_shared<QuestionEvents>();
    ScopedSubscription subscription(entry->loop->events(),
        entry->loop->events().subscribe([observed](const SessionEvent& event) {
            std::lock_guard<std::mutex> lock(observed->mu);
            if (event.kind == SessionEventKind::QuestionRequest) observed->requested = true;
            if (event.kind == SessionEventKind::QuestionClosed)
                observed->closed = event.payload.value("reason", "");
            observed->changed.notify_all();
        }));
    UserInput input;
    input.text = "Ask before continuing.";
    entry->loop->submit(input);
    {
        std::unique_lock<std::mutex> lock(observed->mu);
        ASSERT_TRUE(observed->changed.wait_for(lock, 5s, [&] { return observed->requested; }));
    }
    registry.reset();
    EXPECT_EQ(entry->ask_prompter->pending_count(), 0u);
    EXPECT_FALSE(entry->loop->is_busy());
    EXPECT_TRUE(entry->sm->current_session_id().empty());
    std::lock_guard<std::mutex> lock(observed->mu);
    EXPECT_EQ(observed->closed, "aborted");
}

class AbortAwareProvider : public acecode_test::StubLlmProvider {
public:
    void chat_stream(const std::vector<ChatMessage>&, const std::vector<ToolDef>&,
                     const StreamCallback&, std::atomic<bool>* aborted) override {
        entered.set_value();
        const bool cancelled = wait_until([aborted] { return aborted && aborted->load(); });
        saw_abort.store(cancelled);
        exited.store(true);
    }
    std::promise<void> entered;
    std::atomic<bool> saw_abort{false};
    std::atomic<bool> exited{false};
};

TEST_F(SessionRegistryShutdown, DestructorJoinsRunningTurnWithRetainedEntry) {
    auto registry = make_registry();
    const auto id = registry->create({});
    auto entry = registry->acquire(id);
    auto provider = std::make_shared<AbortAwareProvider>();
    auto entered = provider->entered.get_future();
    install(*entry, provider);
    UserInput input;
    input.text = "Wait for cancellation.";
    entry->loop->submit(input);
    ASSERT_EQ(entered.wait_for(5s), std::future_status::ready);
    registry.reset();
    EXPECT_TRUE(provider->saw_abort.load());
    EXPECT_TRUE(provider->exited.load());
    EXPECT_FALSE(entry->loop->is_busy());
    EXPECT_TRUE(entry->sm->current_session_id().empty());
}

TEST_F(SessionRegistryShutdown, ShutdownIsIdempotentAndRejectsCreateResumeAndTasks) {
    auto registry = make_registry();
    const auto id = registry->create({});
    auto retained = registry->acquire(id);
    registry->shutdown_all();
    registry->shutdown_all();
    EXPECT_EQ(registry->size(), 0u);
    EXPECT_THROW(registry->create({}), std::runtime_error);
    EXPECT_FALSE(registry->resume(id));
    EXPECT_FALSE(registry->enqueue_lifecycle_task([] {}));
    EXPECT_TRUE(retained->sm->current_session_id().empty());
}

TEST_F(SessionRegistryShutdown, CompletedTitleTasksDoNotAccumulate) {
    config.session_title.enabled = true;
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto registry = make_registry([calls](const std::string&) {
        calls->fetch_add(1);
        return std::optional<std::string>("Generated test title");
    });
    for (int i = 0; i != 32; ++i) {
        const auto id = registry->create({});
        UserInput input;
        input.text = "Generate a title";
        registry->maybe_start_auto_title(id, input);
        ASSERT_TRUE(wait_until([&] {
            return calls->load() == i + 1 && registry->background_task_count() == 0;
        }));
        registry->destroy(id);
    }
    EXPECT_EQ(calls->load(), 32);
    EXPECT_EQ(registry->background_task_count(), 0u);
}

TEST_F(SessionRegistryShutdown, DestructorReleasesWriterWithoutChangingActivityTime) {
    auto registry = make_registry();
    const auto id = registry->create({});
    auto retained = registry->acquire(id);
    ChatMessage message;
    message.role = "user";
    message.content = "Persist a session";
    retained->sm->on_message(message);
    const auto project = SessionStorage::get_project_dir(path_to_utf8(isolation.directory.path));
    const auto meta_path = SessionStorage::meta_path(project, id);
    const auto before = SessionStorage::read_meta(meta_path);
    ASSERT_FALSE(before.updated_at.empty());
    ASSERT_TRUE(SessionWriterLease::read(project, id).has_value());
    registry.reset();
    EXPECT_FALSE(SessionWriterLease::read(project, id).has_value());
    EXPECT_EQ(SessionStorage::read_meta(meta_path).updated_at, before.updated_at);
    EXPECT_TRUE(retained->sm->current_session_id().empty());
}

TEST_F(SessionRegistryShutdown, OldClientSubscriptionCannotRemoveReplacementListener) {
    auto registry = make_registry();
    LocalSessionClient client(*registry);
    const auto id = registry->create({});
    auto old_entry = registry->acquire(id);
    auto old_count = std::make_shared<std::atomic<int>>(0);
    ScopedSubscription old_subscription(client, id,
        client.subscribe(id, [old_count](const SessionEvent& event) {
            if (event.kind == SessionEventKind::Token) old_count->fetch_add(1);
        }));
    registry->destroy(id);
    SessionOptions options;
    options.preset_session_id = id;
    ASSERT_EQ(registry->create(options), id);
    auto replacement = registry->acquire(id);
    auto new_count = std::make_shared<std::atomic<int>>(0);
    ScopedSubscription new_subscription(client, id,
        client.subscribe(id, [new_count](const SessionEvent& event) {
            if (event.kind == SessionEventKind::Token) new_count->fetch_add(1);
        }));
    old_subscription.reset();
    old_entry->loop->events().emit(SessionEventKind::Token, {{"text", "old"}});
    replacement->loop->events().emit(SessionEventKind::Token, {{"text", "new"}});
    EXPECT_EQ(old_count->load(), 0);
    EXPECT_EQ(new_count->load(), 1);
}
TEST_F(SessionRegistryShutdown, QueuedPolicyControlsDoNotKeepDestroyedEntryAlive) {
    auto registry = make_registry();
    const auto id = registry->create({});
    auto entry = registry->acquire(id);
    std::weak_ptr<SessionEntry> weak = entry;
    auto provider = std::make_shared<AbortAwareProvider>();
    auto entered = provider->entered.get_future();
    install(*entry, provider);
    UserInput input;
    input.text = "Keep policy changes queued behind this turn.";
    entry->loop->submit(input);
    ASSERT_EQ(entered.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(registry->refresh_sandbox_config(config.sandbox), 1u);
    EXPECT_EQ(registry->refresh_exec_rules(), 1u);
    registry->refresh_mcp_policy(config);
    auto called = std::make_shared<std::atomic<bool>>(false);
    const auto receipt = registry->enqueue_entry_control(entry,
        [called](SessionRegistry&, SessionEntry&) {
            called->store(true);
            return true;
        });
    ASSERT_TRUE(receipt.accepted);
    ASSERT_TRUE(receipt.queued_behind_turn);
    entry.reset();
    registry->destroy(id);
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(called->load());
}
} // namespace
