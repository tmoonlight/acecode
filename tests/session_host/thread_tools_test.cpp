#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/local_session_client.hpp"
#include "session/session_pin_store.hpp"
#include "session/session_registry.hpp"
#include "session/session_storage.hpp"
#include "session/thread_service.hpp"
#include "tool/thread_tools.hpp"
#include "utils/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

acecode::ChatMessage message(std::string role, std::string content) {
    acecode::ChatMessage item;
    item.role = std::move(role);
    item.content = std::move(content);
    return item;
}

std::filesystem::path unique_cwd(const std::string& label) {
    auto cwd = std::filesystem::temp_directory_path() /
        ("acecode_thread_tools_" + label + "_" +
         std::to_string(std::random_device{}()));
    std::filesystem::create_directories(cwd);
    return cwd;
}

// Global discovery must never scan the developer's real session history.
class ThreadTools : public testing::Test {
protected:
    std::filesystem::path root;

    void SetUp() override {
        root = unique_cwd("home");
        if (const char* previous = std::getenv(home_key())) {
            previous_home_ = previous;
            had_home_ = true;
        }
        set_home(root.string());
        previous_mode_ = acecode::override_run_mode_for_test(acecode::RunMode::User);
        acecode::reset_data_dir_cache_for_test();
    }

    void TearDown() override {
        if (had_home_) {
            set_home(previous_home_);
        } else {
#ifdef _WIN32
            _putenv_s(home_key(), "");
#else
            unsetenv(home_key());
#endif
        }
        acecode::override_run_mode_for_test(previous_mode_);
        acecode::reset_data_dir_cache_for_test();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::string workspace(const std::string& name) const {
        const auto path = root / name;
        std::filesystem::create_directories(path);
        return path.string();
    }

private:
    static const char* home_key() {
#ifdef _WIN32
        return "USERPROFILE";
#else
        return "HOME";
#endif
    }
    static void set_home(const std::string& value) {
#ifdef _WIN32
        _putenv_s(home_key(), value.c_str());
#else
        setenv(home_key(), value.c_str(), 1);
#endif
    }
    std::string previous_home_;
    bool had_home_ = false;
    acecode::RunMode previous_mode_ = acecode::RunMode::User;
};

void persist_thread(const std::string& cwd,
                    const std::string& id,
                    const std::string& parent_id,
                    const std::string& content) {
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd);
    acecode::SessionMeta meta;
    meta.id = id;
    meta.cwd = cwd;
    meta.created_at = "2026-08-15T00:00:00Z";
    meta.updated_at = meta.created_at;
    meta.parent_session_id = parent_id;
    ASSERT_TRUE(acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(project_dir, id), meta));
    ASSERT_TRUE(acecode::SessionStorage::append_message(
        acecode::SessionStorage::session_path(project_dir, id),
        message("user", content)));
}

} // namespace

TEST_F(ThreadTools, RegistersCodexAlignedIndependentToolNames) {
    acecode::ToolExecutor tools;
    auto deps = std::make_shared<acecode::ThreadToolDeps>();
    deps->service = std::make_shared<acecode::ThreadService>(
        acecode::ThreadService::Deps{});
    acecode::register_codex_thread_tools(tools, deps);

    const std::unordered_set<std::string> expected{
        "create_thread", "fork_thread", "list_threads", "read_thread",
        "send_message_to_thread", "wait_threads", "set_thread_title",
        "set_thread_pinned", "set_thread_archived", "delete_thread",
        "repair_thread",
    };
    for (const auto& name : expected) {
        EXPECT_TRUE(tools.has_tool(name)) << name;
    }
    EXPECT_TRUE(tools.is_read_only("list_threads"));
    EXPECT_TRUE(tools.is_read_only("read_thread"));
    EXPECT_TRUE(tools.is_read_only("wait_threads"));
    EXPECT_FALSE(tools.is_read_only("delete_thread"));
    EXPECT_FALSE(tools.is_read_only("repair_thread"));

    const auto definitions = tools.get_tool_definitions();
    const auto read = std::find_if(
        definitions.begin(), definitions.end(), [](const auto& definition) {
            return definition.name == "read_thread";
        });
    ASSERT_NE(read, definitions.end());
    const auto& properties = read->parameters["properties"];
    EXPECT_TRUE(properties.contains("threadId"));
    EXPECT_TRUE(properties.contains("turnLimit"));
    EXPECT_TRUE(properties.contains("includeOutputs"));
    EXPECT_TRUE(properties.contains("maxOutputCharsPerItem"));
    EXPECT_TRUE(properties.contains("workspaceHash"));
    EXPECT_FALSE(properties.contains("hostId"));
    EXPECT_FALSE(tools.has_tool("session_query"));
    EXPECT_FALSE(tools.has_tool("session_control"));

    const auto missing_bool = tools.execute(
        "set_thread_pinned", R"({"threadId":"thread"})");
    EXPECT_FALSE(missing_bool.success);
    EXPECT_NE(missing_bool.output.find("pinned is required"),
              std::string::npos);
    const auto missing_id = tools.execute("read_thread", R"({})");
    EXPECT_FALSE(missing_id.success);
    EXPECT_NE(missing_id.output.find("threadId is required"),
              std::string::npos);
}

TEST_F(ThreadTools, ServiceListsPinnedAndReadsBoundedRecentTurns) {
    const auto cwd = unique_cwd("list_read");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);

    {
        acecode::SessionManager manager;
        manager.start_session(cwd_string, "stub", "model");
        manager.on_message(message("user", "first request"));
        manager.on_message(message("assistant", "first answer"));
        manager.on_message(message("user", "latest request"));
        manager.on_message(message("assistant", std::string(500, 'x')));
        manager.set_session_title("Pinned thread");
        const std::string id = manager.current_session_id();

        const auto pin_path = std::filesystem::path(project_dir) /
            "pinned_sessions.json";
        ASSERT_TRUE(acecode::session_pins::write_pinned_sessions_state(
            pin_path, {{id}}));

        acecode::ThreadService service({});
        acecode::ThreadScope scope;
        scope.cwd = cwd_string;
        scope.caller_thread_id = id;
        scope.caller_manager = &manager;

        const auto listed = service.list(scope, 1);
        ASSERT_TRUE(listed.success) << listed.error;
        ASSERT_EQ(listed.value["pinnedThreads"].size(), 1u);
        EXPECT_EQ(listed.value["pinnedThreads"][0]["threadId"], id);

        const auto read = service.read(
            scope, id, {}, 1, false, 256);
        ASSERT_TRUE(read.success) << read.error;
        ASSERT_EQ(read.value["turns"].size(), 1u);
        const auto& items = read.value["turns"][0]["items"];
        ASSERT_EQ(items.size(), 2u);
        EXPECT_EQ(items[0]["content"], "latest request");
        EXPECT_TRUE(items[1]["truncated"].get<bool>());
        EXPECT_FALSE(read.value["nextCursor"].is_null());
        manager.finalize();
    }

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}

TEST_F(ThreadTools, ListsAllProjectsWithPinsArchivesAndPaginationThroughTool) {
    const auto first = workspace("first");
    const auto hidden = workspace("hidden");
    const auto standalone = workspace("standalone");
    const std::string first_pin = "20260918-100000-0001";
    const std::string hidden_pin = "20260918-100001-0002";
    const std::string hidden_pin_two = "20260918-100002-0003";
    const std::string child = "20260918-100003-0004";
    const std::string no_workspace = "20260918-100004-0005";
    const std::string archived = "20260918-100005-0006";
    persist_thread(first, first_pin, {}, "first workspace");
    persist_thread(hidden, hidden_pin, {}, "hidden workspace");
    persist_thread(hidden, hidden_pin_two, {}, "second pinned thread");
    persist_thread(hidden, child, hidden_pin, "child history");
    persist_thread(standalone, no_workspace, {}, "standalone history");
    persist_thread(hidden, archived, {}, "archived history");
    const auto first_project = acecode::SessionStorage::get_project_dir(first);
    const auto hidden_project = acecode::SessionStorage::get_project_dir(hidden);
    {
        std::ofstream marker(std::filesystem::path(hidden_project) / "workspace.json");
        marker << nlohmann::json{{"cwd", hidden}, {"name", "Hidden project"},
                                {"desktop_visible", false}};
    }
    for (const auto& [cwd, id] : std::vector<std::pair<std::string, std::string>>{
             {standalone, no_workspace}, {hidden, archived}}) {
        const auto path = acecode::SessionStorage::meta_path(
            acecode::SessionStorage::get_project_dir(cwd), id);
        auto meta = acecode::SessionStorage::read_meta(path);
        meta.no_workspace = id == no_workspace;
        meta.archived = id == archived;
        ASSERT_TRUE(acecode::SessionStorage::write_meta(path, meta));
    }
    ASSERT_TRUE(acecode::session_pins::write_pinned_sessions_state(
        std::filesystem::path(first_project) / "pinned_sessions.json", {{first_pin}}));
    ASSERT_TRUE(acecode::session_pins::write_pinned_sessions_state(
        std::filesystem::path(hidden_project) / "pinned_sessions.json",
        {{hidden_pin_two, hidden_pin, archived}}));

    acecode::ToolExecutor tools;
    auto deps = std::make_shared<acecode::ThreadToolDeps>();
    deps->service = std::make_shared<acecode::ThreadService>(acecode::ThreadService::Deps{});
    acecode::register_codex_thread_tools(tools, deps);
    acecode::ToolContext context;
    context.cwd = first;
    std::string cursor;
    std::unordered_set<std::string> regular_ids;
    for (int page = 0; page < 3; ++page) {
        const auto result = tools.execute("list_threads",
            nlohmann::json{{"limit", 1}, {"cursor", cursor}}.dump(), context);
        ASSERT_TRUE(result.success) << result.output;
        const auto payload = nlohmann::json::parse(result.output);
        EXPECT_TRUE(payload["errors"].empty());
        ASSERT_EQ(payload["pinnedThreads"].size(), 3u);
        std::vector<std::string> hidden_pins;
        for (const auto& row : payload["pinnedThreads"]) {
            if (row["cwd"] == hidden) {
                hidden_pins.push_back(row["threadId"].get<std::string>());
                EXPECT_EQ(row["workspaceName"], "Hidden project");
                EXPECT_FALSE(row["workspaceVisible"].get<bool>());
            }
        }
        EXPECT_EQ(hidden_pins, (std::vector<std::string>{hidden_pin_two, hidden_pin}));
        ASSERT_EQ(payload["threads"].size(), 1u);
        const auto& row = payload["threads"][0];
        const auto id = row["threadId"].get<std::string>();
        EXPECT_TRUE(regular_ids.insert(id).second);
        EXPECT_FALSE(row["workspaceHash"].get<std::string>().empty());
        EXPECT_EQ(row["noWorkspace"], id == no_workspace);
        if (id == child) EXPECT_EQ(row["parentThreadId"], hidden_pin);
        if (payload["nextCursor"].is_null()) {
            EXPECT_FALSE(payload["hasMore"].get<bool>());
            break;
        }
        EXPECT_TRUE(payload["hasMore"].get<bool>());
        cursor = payload["nextCursor"].get<std::string>();
    }
    EXPECT_EQ(regular_ids, (std::unordered_set<std::string>{child, no_workspace}));

    // No calling cwd is needed, and includeArchived reaches the stored archive.
    const auto result = tools.execute("list_threads", R"({"includeArchived":true})");
    ASSERT_TRUE(result.success) << result.output;
    const auto payload = nlohmann::json::parse(result.output);
    ASSERT_EQ(payload["threads"].size(), 3u);
    const auto archived_row = std::find_if(payload["threads"].begin(),
        payload["threads"].end(), [&](const auto& row) { return row["threadId"] == archived; });
    ASSERT_NE(archived_row, payload["threads"].end());
    EXPECT_TRUE((*archived_row)["archived"].get<bool>());
    EXPECT_FALSE(tools.execute("list_threads", R"({"cursor":"-1"})").success);
}

TEST_F(ThreadTools, ReadsArchivedChildAndWorkspaceFreeHistoryAcrossProjects) {
    const auto source = workspace("source");
    const auto target = workspace("target");
    const std::string id = "20260918-110000-0001";
    persist_thread(target, id, "parent-thread", "history from another workspace");
    const auto meta_path = acecode::SessionStorage::meta_path(
        acecode::SessionStorage::get_project_dir(target), id);
    auto meta = acecode::SessionStorage::read_meta(meta_path);
    meta.archived = true;
    meta.no_workspace = true;
    ASSERT_TRUE(acecode::SessionStorage::write_meta(meta_path, meta));

    acecode::ToolExecutor tools;
    auto deps = std::make_shared<acecode::ThreadToolDeps>();
    deps->service = std::make_shared<acecode::ThreadService>(acecode::ThreadService::Deps{});
    acecode::register_codex_thread_tools(tools, deps);
    acecode::ToolContext context;
    context.cwd = source;
    for (const auto& hash : {std::string{}, acecode::SessionStorage::compute_project_hash(target)}) {
        const auto result = tools.execute("read_thread", nlohmann::json{
            {"threadId", id}, {"workspaceHash", hash}}.dump(), context);
        ASSERT_TRUE(result.success) << result.output;
        const auto payload = nlohmann::json::parse(result.output);
        EXPECT_EQ(payload["cwd"], target);
        EXPECT_TRUE(payload["noWorkspace"].get<bool>());
        EXPECT_EQ(payload["turns"][0]["items"][0]["content"],
                  "history from another workspace");
    }
    // Known ids work even with no calling workspace.
    EXPECT_TRUE(tools.execute("read_thread",
        nlohmann::json{{"threadId", id}}.dump()).success);
    EXPECT_FALSE(tools.execute("read_thread",
        R"({"threadId":"../outside"})").success);
    EXPECT_FALSE(tools.execute("read_thread", nlohmann::json{
        {"threadId", id}, {"workspaceHash", "../outside"}}.dump()).success);
}

TEST_F(ThreadTools, DisambiguatesIdenticalIdsUsingReturnedWorkspaceHash) {
    const auto first = workspace("first");
    const auto second = workspace("second");
    const std::string id = "20260918-120000-0001";
    persist_thread(first, id, {}, "first history");
    persist_thread(second, id, {}, "second history");
    acecode::ThreadService service({});
    acecode::ThreadScope scope;
    scope.cwd = first;
    const auto ambiguous = service.read(scope, id);
    EXPECT_FALSE(ambiguous.success);
    EXPECT_NE(ambiguous.error.find("workspaceHash"), std::string::npos);
    const auto listed = service.list(scope);
    ASSERT_TRUE(listed.success) << listed.error;
    ASSERT_EQ(listed.value["threads"].size(), 2u);
    for (const auto& row : listed.value["threads"]) {
        const auto read = service.read(scope, id, {}, 8, false, 2000,
            row["workspaceHash"].get<std::string>());
        ASSERT_TRUE(read.success) << read.error;
        EXPECT_EQ(read.value["turns"][0]["items"][0]["content"],
                  row["cwd"] == first ? "first history" : "second history");
    }
}

TEST_F(ThreadTools, ReadsAndWaitsForActiveThreadsInAnotherWorkspace) {
    const auto first = workspace("caller");
    const auto second = workspace("active-target");
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::SessionRegistryDeps registry_deps;
    registry_deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    registry_deps.tools = &tools;
    registry_deps.cwd = first;
    registry_deps.template_permissions = &permissions;
    acecode::SessionRegistry registry(std::move(registry_deps));
    acecode::LocalSessionClient client(registry);
    acecode::SessionOptions options;
    options.cwd = second;
    const auto id = registry.create(options);
    auto entry = registry.acquire(id);
    ASSERT_NE(entry, nullptr);
    auto deps = std::make_shared<acecode::ThreadToolDeps>();
    deps->service = std::make_shared<acecode::ThreadService>(
        acecode::ThreadService::Deps{&registry, &client});
    acecode::register_codex_thread_tools(tools, deps);

    acecode::ThreadScope scope;
    scope.cwd = first;
    const auto listed = deps->service->list(scope);
    ASSERT_TRUE(listed.success) << listed.error;
    ASSERT_EQ(listed.value["threads"].size(), 1u);
    EXPECT_EQ(listed.value["threads"][0]["threadId"], id);
    EXPECT_TRUE(listed.value["threads"][0]["active"].get<bool>());
    entry->sm->on_message(message("user", "live history"));
    const auto read = deps->service->read(scope, id);
    ASSERT_TRUE(read.success) << read.error;
    EXPECT_TRUE(read.value["active"].get<bool>());
    EXPECT_EQ(read.value["turns"][0]["items"][0]["content"], "live history");

    const auto seq = entry->loop->events().emit(
        acecode::SessionEventKind::Done, {{"outcome", "success"}});
    acecode::ToolContext context;
    context.cwd = first;
    const auto result = tools.execute("wait_threads", nlohmann::json{
        {"targets", nlohmann::json::array({{
            {"threadId", id}, {"workspaceHash", entry->workspace_hash},
            {"afterCursor", "0"}}})}, {"timeoutMs", 0}}.dump(), context);
    ASSERT_TRUE(result.success) << result.output;
    const auto payload = nlohmann::json::parse(result.output);
    ASSERT_EQ(payload["threads"].size(), 1u);
    EXPECT_TRUE(payload["errors"].empty());
    EXPECT_TRUE(payload["threads"][0]["active"].get<bool>());
    EXPECT_EQ(payload["threads"][0]["cursor"], std::to_string(seq));
    EXPECT_EQ(payload["winnerThreadId"], id);

    scope.cwd = second;
    scope.caller_thread_id = id;
    EXPECT_FALSE(deps->service->wait(scope, {{id, 0}}, 0).success);
    registry.destroy(id);
}

TEST_F(ThreadTools, DeleteCascadesToDescendantsAndCleansPins) {
    const auto cwd = unique_cwd("delete_tree");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);

    const std::string root = "20260815-100000-0001";
    const std::string child = "20260815-100001-0002";
    const std::string grandchild = "20260815-100002-0003";
    const std::string unrelated = "20260815-100003-0004";
    persist_thread(cwd_string, root, {}, "root");
    persist_thread(cwd_string, child, root, "child");
    persist_thread(cwd_string, grandchild, child, "grandchild");
    persist_thread(cwd_string, unrelated, {}, "unrelated");
    const auto pin_path = std::filesystem::path(project_dir) /
        "pinned_sessions.json";
    ASSERT_TRUE(acecode::session_pins::write_pinned_sessions_state(
        pin_path, {{root, child, unrelated}}));

    acecode::ThreadService service({});
    acecode::ThreadScope scope;
    scope.cwd = cwd_string;
    scope.caller_thread_id = "20260815-090000-cafe";
    const auto result = service.delete_thread(scope, root);

    ASSERT_TRUE(result.success) << result.error;
    const std::unordered_set<std::string> deleted(
        result.value["deletedThreadIds"].begin(),
        result.value["deletedThreadIds"].end());
    EXPECT_EQ(deleted, (std::unordered_set<std::string>{
                           root, child, grandchild}));
    for (const auto& id : deleted) {
        EXPECT_TRUE(acecode::SessionStorage::find_session_files(
            project_dir, id).empty());
    }
    EXPECT_FALSE(acecode::SessionStorage::find_session_files(
        project_dir, unrelated).empty());
    const auto pins =
        acecode::session_pins::read_pinned_sessions_state(pin_path);
    EXPECT_EQ(pins.session_ids, std::vector<std::string>{unrelated});

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}

TEST_F(ThreadTools, SelfDeleteWaitsForTuiTurnBoundaryBeforePurging) {
    const auto cwd = unique_cwd("self_delete_tui");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);
    const std::string caller = "20260823-100000-self";
    const std::string child = "20260823-100001-child";

    acecode::SessionManager manager;
    manager.start_session(cwd_string, "stub", "model", caller);
    manager.on_message(message("user", "delete this thread"));
    persist_thread(cwd_string, child, caller, "child work");
    const auto pin_path = std::filesystem::path(project_dir) /
        "pinned_sessions.json";
    ASSERT_TRUE(acecode::session_pins::write_pinned_sessions_state(
        pin_path, {{caller, child}}));

    acecode::ThreadService service({});
    acecode::ThreadScope scope;
    scope.cwd = cwd_string;
    scope.caller_thread_id = caller;
    scope.caller_manager = &manager;
    auto result = service.delete_thread(scope, caller);

    ASSERT_TRUE(result.success) << result.error;
    EXPECT_TRUE(result.terminate_caller_after_turn);
    EXPECT_TRUE(result.value.value("scheduled", false));
    ASSERT_TRUE(static_cast<bool>(result.post_turn_action));
    EXPECT_FALSE(acecode::SessionStorage::find_session_files(
        project_dir, caller).empty());
    EXPECT_FALSE(acecode::SessionStorage::find_session_files(
        project_dir, child).empty());

    result.post_turn_action();

    EXPECT_TRUE(manager.current_session_id().empty());
    EXPECT_TRUE(acecode::SessionStorage::find_session_files(
        project_dir, caller).empty());
    EXPECT_TRUE(acecode::SessionStorage::find_session_files(
        project_dir, child).empty());
    EXPECT_TRUE(acecode::session_pins::read_pinned_sessions_state(
        pin_path).session_ids.empty());

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}

TEST_F(ThreadTools, RegistrySelfDeleteUsesExternalLifecycleWithEntrySnapshotHeld) {
    const auto cwd = unique_cwd("self_delete_registry");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);

    {
        acecode::ToolExecutor registry_tools;
        acecode::PermissionManager registry_permissions;
        acecode::SessionRegistryDeps registry_deps;
        registry_deps.provider_accessor = [] {
            return std::shared_ptr<acecode::LlmProvider>{};
        };
        registry_deps.tools = &registry_tools;
        registry_deps.cwd = cwd_string;
        registry_deps.template_permissions = &registry_permissions;
        acecode::SessionRegistry registry(std::move(registry_deps));
        acecode::LocalSessionClient client(registry);
        acecode::SessionOptions options;
        options.cwd = cwd_string;
        const std::string caller = registry.create(options);
        auto held_entry = registry.acquire(caller);
        ASSERT_NE(held_entry, nullptr);
        ASSERT_NE(held_entry->sm, nullptr);
        held_entry->sm->on_message(message("user", "delete registry thread"));

        acecode::ThreadService service({&registry, &client});
        acecode::ThreadScope scope;
        scope.cwd = cwd_string;
        scope.caller_thread_id = caller;
        scope.caller_manager = held_entry->sm.get();
        auto result = service.delete_thread(scope, caller);
        ASSERT_TRUE(result.success) << result.error;
        ASSERT_TRUE(static_cast<bool>(result.post_turn_action));

        result.post_turn_action();
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline &&
               (registry.acquire(caller) ||
                !acecode::SessionStorage::find_session_files(
                    project_dir, caller).empty())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        EXPECT_EQ(registry.acquire(caller), nullptr);
        EXPECT_TRUE(acecode::SessionStorage::find_session_files(
            project_dir, caller).empty());
        // Keeping the entry snapshot alive must not keep its worker running or
        // allow persistent cleanup to race ahead of shutdown.
        ASSERT_NE(held_entry->loop, nullptr);
        EXPECT_TRUE(held_entry->sm->current_session_id().empty());
        held_entry.reset();
    } // SessionRegistry joins the deferred lifecycle task before cleanup.

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}

TEST_F(ThreadTools, HealthyThreadCanRepairBlockedInactiveThread) {
    const auto cwd = unique_cwd("repair_inactive");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);
    const std::string target = "20260815-110000-beef";

    {
        acecode::SessionManager manager;
        manager.start_session(cwd_string, "stub", "model", target);
        manager.on_message(message("user", "old request"));
        manager.on_message(message("assistant", "old answer"));
        manager.on_message(message("user", "blocked current request"));
        manager.finalize();
    }

    acecode::ThreadService service({});
    acecode::ThreadScope scope;
    scope.cwd = cwd_string;
    scope.caller_thread_id = "20260815-105900-cafe";
    const auto repaired = service.repair(scope, target);

    ASSERT_TRUE(repaired.success) << repaired.error;
    EXPECT_EQ(repaired.value["status"], "repaired");
    EXPECT_EQ(repaired.value["threadId"], target);
    const auto files = acecode::SessionStorage::find_session_files(
        project_dir, target);
    ASSERT_EQ(files.size(), 1u);
    const auto raw = acecode::SessionStorage::load_messages(
        files.front().jsonl_path);
    ASSERT_EQ(raw.size(), 4u);
    EXPECT_TRUE(acecode::is_compact_checkpoint_message(raw.back()));
    const auto effective =
        acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 1u);
    EXPECT_EQ(effective.front().content, "blocked current request");

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}

TEST_F(ThreadTools, HealthyThreadCanRepairBlockedActiveThreadAtQueueBoundary) {
    const auto cwd = unique_cwd("repair_active");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);

    acecode::ToolExecutor registry_tools;
    acecode::PermissionManager registry_permissions;
    acecode::SessionRegistryDeps registry_deps;
    registry_deps.provider_accessor = [] {
        return std::shared_ptr<acecode::LlmProvider>{};
    };
    registry_deps.tools = &registry_tools;
    registry_deps.cwd = cwd_string;
    registry_deps.template_permissions = &registry_permissions;
    acecode::SessionRegistry registry(std::move(registry_deps));
    acecode::LocalSessionClient client(registry);
    acecode::SessionOptions options;
    options.cwd = cwd_string;
    const std::string target = registry.create(options);
    auto entry = registry.acquire(target);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->loop, nullptr);
    ASSERT_NE(entry->sm, nullptr);
    const std::vector<acecode::ChatMessage> history{
        message("user", "old request"),
        message("assistant", "old answer"),
        message("user", "blocked current request"),
    };
    for (const auto& item : history) {
        entry->loop->push_message(item);
        entry->sm->on_message(item);
    }

    acecode::ThreadService service({&registry, &client});
    acecode::ThreadScope scope;
    scope.cwd = cwd_string;
    scope.caller_thread_id = "20260815-115900-cafe";
    const auto repaired = service.repair(scope, target);

    ASSERT_TRUE(repaired.success) << repaired.error;
    EXPECT_EQ(repaired.value["status"], "repaired");
    ASSERT_EQ(entry->loop->messages().size(), 1u);
    EXPECT_EQ(entry->loop->messages().front().content,
              "blocked current request");
    const auto raw = entry->sm->load_active_messages();
    ASSERT_EQ(raw.size(), 4u);
    EXPECT_TRUE(acecode::is_compact_checkpoint_message(raw.back()));

    const auto self_send = service.send(
        scope, scope.caller_thread_id, "must not queue");
    EXPECT_FALSE(self_send.success);
    ASSERT_TRUE(service.set_archived(scope, target, true).success);
    const auto listed = service.list(scope, 20);
    ASSERT_TRUE(listed.success) << listed.error;
    for (const char* section : {"pinnedThreads", "threads"}) {
        for (const auto& item : listed.value[section]) {
            EXPECT_NE(item["threadId"], target);
        }
    }

    registry.destroy(target);
    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}
