#include <gtest/gtest.h>

#include "session/compact_checkpoint.hpp"
#include "session/local_session_client.hpp"
#include "session/session_registry.hpp"
#include "session/task_suggestion_service.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/thread_goal_store.hpp"
#include "utils/utf8_path.hpp"
#include "worktree/worktree_manager.hpp"
#include "web/message_payload.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

acecode::ChatMessage chat(std::string role, std::string text, std::string id = {}) {
    acecode::ChatMessage message;
    message.role = std::move(role);
    message.content = std::move(text);
    message.uuid = std::move(id);
    return message;
}

class RecordingClient : public acecode::LocalSessionClient {
public:
    explicit RecordingClient(acecode::SessionRegistry& registry)
        : LocalSessionClient(registry), registry_(registry) {}

    bool send_input(const std::string& id, const acecode::UserInput& input) override {
        return record_input(registry_.acquire(id), input);
    }

    bool record_input(const std::shared_ptr<acecode::SessionEntry>& entry,
                       const acecode::UserInput& input) {
        std::lock_guard<std::mutex> lock(mutex);
        ++attempts;
        if (fail_next) { fail_next = false; changed.notify_all(); return false; }
        if (!entry) return false;
        auto message = chat("user", input.text, "receipt-" + std::to_string(attempts));
        message.metadata = input.metadata;
        message.metadata["display_text"] = input.display_text;
        entry->sm->on_message(message);
        ids.push_back(entry->id);
        inputs.push_back(input);
        changed.notify_all();
        return true;
    }

    bool wait_for_inputs(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return inputs.size() >= count; });
    }

    std::size_t input_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return inputs.size();
    }

    acecode::SessionRegistry& registry_;
    std::mutex mutex;
    std::condition_variable changed;
    bool fail_next = false;
    int attempts = 0;
    std::vector<std::string> ids;
    std::vector<acecode::UserInput> inputs;
};

struct WorkerGate {
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();

    void hold(acecode::AgentLoop& loop) {
        const auto shared = state;
        ASSERT_TRUE(loop.enqueue_control([shared] {
            std::unique_lock<std::mutex> lock(shared->mutex);
            shared->entered = true;
            shared->changed.notify_all();
            shared->changed.wait_for(lock, 10s, [&] { return shared->released; });
            return true;
        }).accepted);
        std::unique_lock<std::mutex> lock(shared->mutex);
        ASSERT_TRUE(shared->changed.wait_for(lock, 3s, [&] { return shared->entered; }));
    }

    void release() {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->released = true;
        state->changed.notify_all();
    }
    ~WorkerGate() { release(); }
};

class CompletingProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                              const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse result;
        result.content = "The accepted task is complete.";
        result.finish_reason = "stop";
        return result;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(messages);
            changed.notify_all();
        }
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "The accepted task is complete.";
        callback(delta);
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        done.finish_reason = "stop";
        callback(done);
    }
    std::string name() const override { return "suggestion-test"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "suggestion-model"; }
    void set_model(const std::string&) override {}

    bool wait_for_request() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return !requests.empty(); });
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::vector<acecode::ChatMessage>> requests;
};

class TaskSuggestionServiceTest : public testing::Test {
protected:
    void SetUp() override {
        cwd = fs::temp_directory_path() /
            ("acecode-suggestions-" + std::to_string(std::random_device{}()));
        fs::create_directories(cwd);
        acecode::SessionRegistryDeps deps;
        deps.cwd = acecode::path_to_utf8(cwd);
        deps.tools = &tools;
        deps.template_permissions = &permissions;
        deps.no_workspace_cache_root = acecode::path_to_utf8(cwd / "no-workspace");
        deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
        registry = std::make_unique<acecode::SessionRegistry>(deps);
        client = std::make_unique<RecordingClient>(*registry);
        acecode::TaskSuggestionService::Deps service_deps{registry.get(), client.get()};
        service_deps.enqueue_input = [this](const auto& entry, const auto& input) {
            return client->record_input(entry, input);
        };
        service = std::make_unique<acecode::TaskSuggestionService>(service_deps);
        acecode::SessionOptions options;
        options.cwd = acecode::path_to_utf8(cwd);
        options.permission_mode = "auto";
        source_id = registry->create(options);
        source = registry->acquire(source_id);
        ASSERT_NE(source, nullptr);
        project_dir = source->sm->current_project_dir();
        project_dirs.push_back(project_dir);
        source->sm->on_message(chat("user", "Preserve unrelated dirty files.", "original"));
        source->sm->on_message(chat("assistant", "The initial check is complete.", "check"));
    }

    void TearDown() override {
        service->shutdown();
        service.reset();
        source.reset();
        client.reset();
        registry.reset();
        std::error_code ignored;
        for (const auto& project : project_dirs) {
            fs::remove_all(acecode::path_from_utf8(project), ignored);
        }
        fs::remove_all(cwd, ignored);
    }

    json propose(bool handoff = false) {
        json draft{{"kind", handoff ? "context_handoff" : "side_task"},
                   {"title", "Fix a separate issue"},
                   {"description", "An independent finding with a verified file."},
                   {"prompt", "Fix the independent issue and run its focused test."}};
        if (handoff) {
            acecode::TaskSuggestionStore store(acecode::path_from_utf8(project_dir));
            std::string error;
            const auto result = store.propose(source_id, draft, &error);
            EXPECT_TRUE(result.has_value()) << error;
            return result.value_or(json::object());
        }
        const auto result = service->propose(source_id, draft);
        EXPECT_TRUE(result.ok) << result.error;
        return result.value.value("suggestion", json::object());
    }

    json await_status(const std::string& id, const std::string& status) {
        acecode::TaskSuggestionStore store(acecode::path_from_utf8(project_dir));
        const auto deadline = std::chrono::steady_clock::now() + 6s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto record = store.get(source_id, id);
            if (record && record->value("status", "") == status) return *record;
            std::this_thread::sleep_for(10ms);
        }
        const auto record = store.get(source_id, id);
        ADD_FAILURE() << "Expected " << status << ": " << (record ? record->dump() : "missing");
        return record.value_or(json::object());
    }

    void init_git() {
        const auto path = acecode::path_to_utf8(cwd);
        ASSERT_TRUE(acecode::worktree::run_git({"init", "-b", "main"}, path).ok());
        std::ofstream(cwd / "tracked.txt") << "committed\n";
        ASSERT_TRUE(acecode::worktree::run_git({"add", "tracked.txt"}, path).ok());
        ASSERT_TRUE(acecode::worktree::run_git({"-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                                               "commit", "-m", "baseline"}, path).ok());
    }

    fs::path cwd;
    std::string project_dir;
    std::vector<std::string> project_dirs;
    std::string source_id;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    std::unique_ptr<acecode::SessionRegistry> registry;
    std::unique_ptr<RecordingClient> client;
    std::unique_ptr<acecode::TaskSuggestionService> service;
    std::shared_ptr<acecode::SessionEntry> source;
};

TEST_F(TaskSuggestionServiceTest, ProposalDoesNotStartAndDoubleAcceptUsesOneTarget) {
    const auto suggestion = propose();
    EXPECT_EQ(registry->size(), 1u);
    const auto first = service->accept(source_id, suggestion["id"], "current_branch");
    const auto second = service->accept(source_id, suggestion["id"], "current_branch");
    ASSERT_TRUE(first.ok) << first.error;
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(first.value["suggestion"]["target_session_id"],
              second.value["suggestion"]["target_session_id"]);
    ASSERT_TRUE(client->wait_for_inputs(1));
    await_status(suggestion["id"], "started");
    const auto third = service->accept(source_id, suggestion["id"], "current_branch");
    EXPECT_TRUE(third.ok);
    EXPECT_EQ(client->input_count(), 1u);
    EXPECT_EQ(registry->size(), 2u);
    const auto target = registry->acquire(first.value["suggestion"]["target_session_id"]);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->loop->cwd(), acecode::path_to_utf8(cwd));
    EXPECT_EQ(target->sm->current_permission_mode(), "auto");
}

TEST_F(TaskSuggestionServiceTest, SharedDirectoryWaitsForSourceAndCanBeDismissed) {
    WorkerGate gate;
    gate.hold(*source->loop);
    const auto suggestion = propose();
    ASSERT_TRUE(service->accept(source_id, suggestion["id"], "current_branch").ok);
    std::this_thread::sleep_for(350ms);
    EXPECT_EQ(client->input_count(), 0u);
    const auto dismissed = service->dismiss(source_id, suggestion["id"]);
    ASSERT_TRUE(dismissed.ok) << dismissed.error;
    gate.release();
    std::this_thread::sleep_for(350ms);
    EXPECT_EQ(client->input_count(), 0u);
    EXPECT_EQ(registry->size(), 1u);
}

TEST_F(TaskSuggestionServiceTest, SharedDirectoryWaitsForOtherKnownSession) {
    acecode::SessionOptions options;
    options.cwd = acecode::path_to_utf8(cwd);
    const auto other = registry->acquire(registry->create(options));
    ASSERT_NE(other, nullptr);
    WorkerGate gate;
    gate.hold(*other->loop);
    const auto suggestion = propose();
    ASSERT_TRUE(service->accept(source_id, suggestion["id"], "current_branch").ok);
    std::this_thread::sleep_for(350ms);
    EXPECT_EQ(client->input_count(), 0u);
    gate.release();
    ASSERT_TRUE(client->wait_for_inputs(1));
    await_status(suggestion["id"], "started");
}

TEST_F(TaskSuggestionServiceTest, IdlePlanGoalDoesNotPreventSideTaskOrChangeGoalState) {
    ASSERT_TRUE(registry->set_permission_mode(source_id, acecode::PermissionMode::Plan));
    auto* goals = source->sm->goal_store();
    ASSERT_NE(goals, nullptr);
    std::string error;
    ASSERT_TRUE(goals->replace_thread_goal(source_id, "Prepare the main implementation plan",
        std::nullopt, acecode::ThreadGoalStatus::Active, &error)) << error;
    const auto suggestion = propose();
    ASSERT_TRUE(service->accept(source_id, suggestion["id"], "current_branch").ok);
    ASSERT_TRUE(client->wait_for_inputs(1));
    await_status(suggestion["id"], "started");
    const auto unchanged = goals->get_thread_goal(source_id);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->status, acecode::ThreadGoalStatus::Active);
    EXPECT_EQ(unchanged->objective, "Prepare the main implementation plan");
}

TEST_F(TaskSuggestionServiceTest, RetryKeepsTargetAndRecoveryDoesNotRepeatInput) {
    const auto suggestion = propose();
    client->fail_next = true;
    const auto accepted = service->accept(source_id, suggestion["id"], "current_branch");
    ASSERT_TRUE(accepted.ok);
    const auto failed = await_status(suggestion["id"], "failed");
    EXPECT_EQ(failed["target_session_id"], accepted.value["suggestion"]["target_session_id"]);
    ASSERT_TRUE(service->accept(source_id, suggestion["id"], "current_branch").ok);
    ASSERT_TRUE(client->wait_for_inputs(1));
    await_status(suggestion["id"], "started");
    acecode::TaskSuggestionStore store(acecode::path_from_utf8(project_dir));
    ASSERT_TRUE(store.update(source_id, suggestion["id"], [](json& record) {
        record["status"] = "starting";
        return true;
    }).has_value());
    ASSERT_TRUE(service->recover(source_id).ok);
    await_status(suggestion["id"], "started");
    EXPECT_EQ(client->input_count(), 1u);
    EXPECT_EQ(registry->size(), 2u);
}

TEST_F(TaskSuggestionServiceTest, WorktreeRunsWhileSourceBusyWithoutCopyingDirtyFiles) {
    init_git();
    std::ofstream(cwd / "tracked.txt") << "uncommitted\n";
    std::ofstream(cwd / "untracked.txt") << "local only\n";
    WorkerGate gate;
    gate.hold(*source->loop);
    const auto suggestion = propose();
    const auto accepted = service->accept(source_id, suggestion["id"], "worktree");
    ASSERT_TRUE(accepted.ok) << accepted.error;
    ASSERT_TRUE(client->wait_for_inputs(1));
    const auto started = await_status(suggestion["id"], "started");
    const auto target = registry->acquire(started["target_session_id"]);
    ASSERT_NE(target, nullptr);
    EXPECT_NE(target->loop->cwd(), acecode::path_to_utf8(cwd));
    const auto target_path = acecode::path_from_utf8(target->loop->cwd());
    EXPECT_FALSE(fs::exists(target_path / "untracked.txt"));
    std::ifstream tracked(target_path / "tracked.txt");
    std::string line;
    std::getline(tracked, line);
    EXPECT_EQ(line, "committed");
    EXPECT_FALSE(target->sm->active_worktree().inherited);
    EXPECT_EQ(target->sm->active_worktree().original_head_commit,
              started["launch_context"]["source_head"]);
    gate.release();
}

TEST_F(TaskSuggestionServiceTest, CurrentDirectoryInheritsExistingWorktreeAndRecentHandoff) {
    init_git();
    const auto created = acecode::worktree::get_or_create_worktree(acecode::path_to_utf8(cwd), "source");
    ASSERT_TRUE(created.ok) << created.error;
    acecode::WorktreeSessionInfo wt;
    wt.original_cwd = acecode::path_to_utf8(cwd);
    wt.worktree_path = created.worktree_path;
    wt.worktree_name = "source";
    wt.worktree_branch = created.worktree_branch;
    wt.original_head_commit = created.head_commit;
    source->sm->set_active_worktree(wt);
    source->loop->set_cwd(wt.worktree_path);
    const auto suggestion = propose(true);
    source->sm->on_message(chat("assistant", "A later focused test passed after the reminder.", "later"));
    const auto accepted = service->accept(source_id, suggestion["id"], "current_branch");
    ASSERT_TRUE(accepted.ok) << accepted.error;
    ASSERT_TRUE(client->wait_for_inputs(1));
    const auto started = await_status(suggestion["id"], "started");
    const auto target = registry->acquire(started["target_session_id"]);
    ASSERT_NE(target, nullptr);
    EXPECT_EQ(target->loop->cwd(), wt.worktree_path);
    EXPECT_TRUE(target->sm->active_worktree().inherited);
    EXPECT_TRUE(source->sm->active_worktree().inherited);
    ASSERT_EQ(client->inputs.size(), 1u);
    EXPECT_NE(client->inputs[0].text.find("later focused test passed"), std::string::npos);
    EXPECT_EQ(client->inputs[0].metadata["session_references"][0]["session_id"], source_id);
    EXPECT_EQ(client->inputs[0].metadata["continued_from"], source_id);
    EXPECT_EQ(target->sm->load_active_messages().size(), 1u);
    EXPECT_GE(source->sm->load_active_messages().size(), 3u);
}

TEST_F(TaskSuggestionServiceTest, NoWorkspaceContinuationKeepsExecutionAndHistoryScope) {
    acecode::SessionOptions options;
    options.no_workspace = true;
    source_id = registry->create(options);
    source = registry->acquire(source_id);
    ASSERT_NE(source, nullptr);
    project_dir = source->sm->current_project_dir();
    project_dirs.push_back(project_dir);
    source->sm->on_message(chat("user", "Continue the task without a project.", "task-request"));
    const auto original_cwd = source->cwd;
    const auto suggestion = propose(true);
    ASSERT_TRUE(service->accept(source_id, suggestion["id"], "current_branch").ok);
    ASSERT_TRUE(client->wait_for_inputs(1));
    const auto started = await_status(suggestion["id"], "started");
    const auto target = registry->acquire(started["target_session_id"]);
    ASSERT_NE(target, nullptr);
    EXPECT_TRUE(target->no_workspace);
    EXPECT_EQ(target->cwd, original_cwd);
    EXPECT_EQ(target->loop->cwd(), original_cwd);
    EXPECT_EQ(target->sm->current_project_dir(), project_dir);
    EXPECT_TRUE(started["target_session"]["no_workspace"].get<bool>());
    EXPECT_EQ(started["target_session"]["working_cwd"], original_cwd);
}

TEST_F(TaskSuggestionServiceTest, ProductionDispatchRunsModelAndPersistsSourceReferenceOnce) {
    service->shutdown();
    const auto provider = std::make_shared<CompletingProvider>();
    acecode::SessionModelState state;
    state.provider = provider->name();
    state.model = provider->model();
    source->model_binding->install_runtime_snapshot(provider, state, 0);
    source->sm->set_active_provider(state.provider, state.model, state.name);
    service = std::make_unique<acecode::TaskSuggestionService>(
        acecode::TaskSuggestionService::Deps{registry.get(), client.get()});
    const auto suggestion = propose();
    const auto accepted = service->accept(source_id, suggestion["id"], "current_branch");
    ASSERT_TRUE(accepted.ok) << accepted.error;
    ASSERT_TRUE(provider->wait_for_request());
    const auto started = await_status(suggestion["id"], "started");
    const auto target = registry->acquire(started["target_session_id"]);
    ASSERT_NE(target, nullptr);
    const auto messages = target->sm->load_active_messages();
    std::size_t receipts = 0;
    for (const auto& message : messages) {
        if (message.role == "user" && message.metadata.is_object() &&
            message.metadata.value("task_suggestion_id", "") == suggestion["id"]) {
            ++receipts;
            EXPECT_EQ(message.metadata["session_references"][0]["session_id"], source_id);
            EXPECT_NE(message.metadata.value("display_text", "").find("@"), std::string::npos);
        }
    }
    EXPECT_EQ(receipts, 1u);
    EXPECT_TRUE(service->accept(source_id, suggestion["id"], "current_branch").ok);
    std::this_thread::sleep_for(300ms);
    std::lock_guard<std::mutex> lock(provider->mutex);
    EXPECT_EQ(provider->requests.size(), 1u);
}

TEST_F(TaskSuggestionServiceTest, RestartRecoversHiddenStartedInputBeforeItsDurableReceipt) {
    const auto suggestion = propose();
    service->shutdown();
    acecode::TaskSuggestionStore store(acecode::path_from_utf8(project_dir));
    const auto claim = store.claim(source_id, suggestion["id"], "current_branch");
    ASSERT_TRUE(claim.suggestion.has_value());
    const auto target_id = claim.suggestion->value("target_session_id", "");
    ASSERT_TRUE(store.update(source_id, suggestion["id"], [](json& record) {
        record["status"] = "started";
        record["input_accepted"] = true;
        record["input_persisted"] = false;
        return true;
    }).has_value());
    ASSERT_TRUE(store.dismiss(source_id, suggestion["id"]).has_value());
    const auto provider = std::make_shared<CompletingProvider>();
    acecode::SessionModelState state;
    state.provider = provider->name();
    state.model = provider->model();
    source->model_binding->install_runtime_snapshot(provider, state, 0);
    source->sm->set_active_provider(state.provider, state.model, state.name);
    // A fresh service discovers already accepted outbox records, including
    // hidden cards. It does not require another accept or a polling GET.
    service = std::make_unique<acecode::TaskSuggestionService>(
        acecode::TaskSuggestionService::Deps{registry.get(), client.get()});
    ASSERT_TRUE(provider->wait_for_request());
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::optional<json> delivered;
    do {
        delivered = store.get(source_id, suggestion["id"]);
        if (delivered && delivered->value("input_persisted", false)) break;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(delivered.has_value());
    EXPECT_EQ((*delivered)["status"], "dismissed");
    EXPECT_TRUE(delivered->value("input_persisted", false));
    EXPECT_EQ((*delivered)["target_session_id"], target_id);
    EXPECT_EQ(registry->size(), 2u);
    std::lock_guard<std::mutex> lock(provider->mutex);
    EXPECT_EQ(provider->requests.size(), 1u);
}

TEST(TaskHandoffSnapshot, KeepsLatestSummaryAndPostSummaryProgressWithinBound) {
    std::vector<acecode::ChatMessage> history;
    history.push_back(chat("user", "Never overwrite unrelated work.", "first"));
    for (int index = 0; index < 100; ++index) {
        history.push_back(chat("assistant", std::string(2000, 'x'), "old"));
    }
    acecode::CompactCheckpoint checkpoint;
    checkpoint.id = "checkpoint";
    checkpoint.trigger = "manual";
    checkpoint.summary = "The objective is implement feature A; tests B remain.";
    history.push_back(acecode::encode_compact_checkpoint(checkpoint));
    history.push_back(chat("user", "Also preserve the exact Chinese wording.", "constraint"));
    history.push_back(chat("assistant", "Tests B passed; only verification C remains.", "latest"));
    const auto snapshot = acecode::build_task_handoff_snapshot(
        {{"source_session_id", "source"}}, history, {{"objective", "Feature A"}}, json::array());
    EXPECT_EQ(snapshot["summary_checkpoint_id"], "checkpoint");
    EXPECT_EQ(snapshot["source_cutoff_message_id"], acecode::web::compute_message_id(history.back()));
    EXPECT_EQ(snapshot["recent_updates"].size(), 2u);
    EXPECT_NE(snapshot.dump().find("Tests B passed"), std::string::npos);
    EXPECT_NE(snapshot.dump().find("Never overwrite unrelated work"), std::string::npos);
    EXPECT_LT(snapshot.dump().size(), 40000u);
}

TEST(TaskHandoffSnapshot, RepairAndHiddenGoalInputsDoNotReplaceSemanticContext) {
    std::vector<acecode::ChatMessage> history{chat("user", "Keep the requested behavior.", "first")};
    acecode::CompactCheckpoint semantic;
    semantic.id = "semantic";
    semantic.trigger = "auto";
    semantic.summary = "Meaningful decisions and remaining tasks.";
    history.push_back(acecode::encode_compact_checkpoint(semantic));
    history.push_back(chat("assistant", "Progress after the semantic summary.", "progress"));
    auto hidden = chat("user", std::string(15000, 'h'), "hidden-goal");
    hidden.metadata = {{"hidden_goal_context", true}};
    history.push_back(hidden);
    acecode::CompactCheckpoint repair;
    repair.id = "repair";
    repair.trigger = "repair-overflow";
    repair.summary = "Deterministic thread repair";
    history.push_back(acecode::encode_compact_checkpoint(repair));
    const auto snapshot = acecode::build_task_handoff_snapshot({}, history, {}, {});
    EXPECT_EQ(snapshot["summary_checkpoint_id"], "semantic");
    EXPECT_EQ(snapshot["source_cutoff_message_id"], acecode::web::compute_message_id(history[2]));
    EXPECT_NE(snapshot.dump().find("Progress after the semantic summary"), std::string::npos);
    EXPECT_EQ(snapshot.dump().find("hidden-goal"), std::string::npos);
    EXPECT_EQ(snapshot.dump().find("Deterministic thread repair"), std::string::npos);
}

} // namespace
