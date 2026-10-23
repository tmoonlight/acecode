#include <gtest/gtest.h>
#include <cpr/cpr.h>

#include "web/server.hpp"
#include "desktop/daemon_supervisor.hpp"
#include "session/sidebar_tree.hpp"
#include "loop/loop_store.hpp"
#include "session/session_pin_store.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include "workspace/workspace_registry.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "utils/cwd_hash.hpp"
#include "utils/joining_thread.hpp"

#include <chrono>
#include <fstream>
#include <thread>

namespace {
using namespace acecode;
using nlohmann::json;
using namespace std::chrono_literals;

class SidebarSessionsHttp : public testing::Test {
protected:
    acecode_test::MemoryTestHome home{"sidebar-http"};
    std::string cwd = home.workspace_cwd("workspace");
    std::string projects = (home.root() / ".acecode" / "projects").string();
    std::string cache = (home.root() / "no-workspace").string();
    AppConfig config;
    ToolExecutor tools;
    desktop::WorkspaceRegistry workspaces;
    loop::LoopStore loops{home.root() / "loops.sqlite3"};
    std::unique_ptr<SessionRegistry> registry;
    std::unique_ptr<LocalSessionClient> client;
    std::unique_ptr<web::WebServer> server;
    JoiningThread worker;

    void SetUp() override {
        ASSERT_TRUE(loops.initialize());
        config.web.bind = "127.0.0.1";
        config.web.port = desktop::pick_free_loopback_port();
        workspaces.register_new(projects, cwd);
        SessionRegistryDeps session_deps;
        session_deps.cwd = cwd;
        session_deps.tools = &tools;
        session_deps.no_workspace_cache_root = cache;
        registry = std::make_unique<SessionRegistry>(std::move(session_deps));
        client = std::make_unique<LocalSessionClient>(*registry);
        web::WebServerDeps deps;
        deps.web_cfg = &config.web;
        deps.app_config = &config;
        deps.cwd = cwd;
        deps.config_path = (home.root() / "config.json").string();
        deps.sync_model_reasoning = false;
        deps.token = "sidebar-fixture";
        deps.projects_dir = projects;
        deps.no_workspace_cache_root = cache;
        deps.workspace_registry = &workspaces;
        deps.session_client = client.get();
        deps.session_registry = registry.get();
        deps.loop_store = &loops;
        server = std::make_unique<web::WebServer>(std::move(deps));
        worker = JoiningThread([this] { server->run(); });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cpr::Get(cpr::Url{url("/api/health")}, cpr::Timeout{200}).status_code == 200) return;
            std::this_thread::sleep_for(10ms);
        }
        FAIL() << "sidebar HTTP fixture did not start";
    }
    void TearDown() override {
        if (server) server->stop();
        worker.join();
        server.reset();
        client.reset();
        registry.reset();
    }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(config.web.port) + path;
    }
    std::string endpoint() const { return "/api/workspaces/" + compute_cwd_hash(cwd) + "/session-sidebar"; }
    void record(const std::string& id, const std::string& loop = {}) {
        SessionMeta meta;
        meta.id = id; meta.cwd = cwd; meta.title = id;
        meta.created_at = id; meta.updated_at = id; meta.loop_id = loop;
        const auto directory = SessionStorage::get_project_dir(cwd);
        std::filesystem::create_directories(directory);
        ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(directory, id), meta));
    }
    json get(const cpr::Parameters& query = {}) {
        auto response = cpr::Get(cpr::Url{url(endpoint())}, query, cpr::Timeout{3000});
        EXPECT_EQ(response.status_code, 200) << response.text;
        return json::parse(response.text);
    }
    cpr::Response move(const std::string& source, const std::string& target, const std::string& placement) {
        return cpr::Post(cpr::Url{url(endpoint())}, cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{json{{"source", source}, {"target", target}, {"placement", placement}}.dump()}, cpr::Timeout{3000});
    }
};

TEST_F(SidebarSessionsHttp, BatchMovePersistsOrderAndRejectsPartialOrMalformedRequests) {
    record("run-1", "a"); record("run-2", "a"); record("receiver", "b");
    const auto post = [&](const json& sources) {
        return cpr::Post(cpr::Url{url(endpoint())}, cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{json{{"sources", sources}, {"target", "loop:b"}, {"placement", "inside"}}.dump()},
            cpr::Timeout{3000});
    };
    auto response = post(json::array({"session:run-2", "session:run-1"}));
    ASSERT_EQ(response.status_code, 200) << response.text;
    const auto page = get({{"expanded", "[\"loop:b\"]"}});
    ASSERT_EQ(page["sessions"].size(), 4u);
    EXPECT_EQ(page["sessions"][2]["id"], "run-2");
    EXPECT_EQ(page["sessions"][3]["id"], "run-1");
    const auto layout_path = std::filesystem::path(SessionStorage::get_project_dir(cwd)) / "sidebar_sessions.json";
    const auto saved = read_sidebar_layout(layout_path);
    for (const auto& sources : {json::array({"session:run-1", "missing"}), json::array(),
                               json::array({"session:run-1", 42}), json("wrong")}) {
        EXPECT_EQ(post(sources).status_code, 409);
        EXPECT_EQ(read_sidebar_layout(layout_path), saved);
    }
}

TEST_F(SidebarSessionsHttp, PagesGroupsAndReturnsAllExpandedHistory) {
    for (int i = 10; i < 23; ++i) record("run-" + std::to_string(i), "daily");
    for (int i = 0; i < 6; ++i) record("normal-" + std::to_string(i));
    auto page = get({{"limit", "5"}});
    EXPECT_EQ(page["total"], 7);
    EXPECT_EQ(page["sessions"].size(), 5u);
    EXPECT_TRUE(page["has_more"].get<bool>());
    EXPECT_EQ(page["sessions"][0]["id"], "sidebar-group:loop:daily");
    EXPECT_EQ(page["sessions"][0]["sidebar_latest_session_id"], "run-22");
    EXPECT_EQ(page["sessions"][0]["sidebar_members"].size(), 13u);
    page = get({{"limit", "5"}, {"expanded", "[\"loop:daily\"]"}});
    EXPECT_EQ(page["sessions"].size(), 18u);
    EXPECT_EQ(page["visible_roots"], 5);
    EXPECT_EQ(page["sessions"][1]["id"], "run-22");
    EXPECT_EQ(page["sessions"][13]["id"], "run-10");
    page = get({{"limit", "5"}, {"reveal", "run-10"}});
    EXPECT_EQ(page["sessions"].size(), 18u);
    page = get({{"limit", "5"}, {"reveal", "normal-0"}});
    EXPECT_FALSE(page["has_more"].get<bool>());
    EXPECT_EQ(page["visible_roots"], 7);
}

TEST_F(SidebarSessionsHttp, DetachPersistsAndRejectsThirdLevelWithoutChangingData) {
    record("run-1", "a"); record("run-2", "a"); record("receiver", "b"); record("other");
    auto response = move("session:run-1", "loop:b", "before");
    ASSERT_EQ(response.status_code, 200) << response.text;
    record("run-3", "a");
    auto page = get();
    EXPECT_EQ(page["total"], 4);
    response = move("session:run-1", "loop:b", "inside");
    ASSERT_EQ(response.status_code, 200) << response.text;
    page = get({{"expanded", "[\"loop:b\"]"}});
    bool associated = false;
    for (const auto& row : page["sessions"]) {
        if (row["id"] == "run-1") {
            EXPECT_EQ(row["sidebar_parent_key"], "loop:b");
            EXPECT_EQ(row["loop_execution"]["loop_id"], "a");
            associated = true;
        }
    }
    EXPECT_TRUE(associated);
    const auto before = read_sidebar_layout(std::filesystem::path(SessionStorage::get_project_dir(cwd)) / "sidebar_sessions.json");
    EXPECT_EQ(move("loop:b", "loop:a", "inside").status_code, 409);
    EXPECT_EQ(move("session:other", "session:run-1", "inside").status_code, 409);
    EXPECT_EQ(move("loop:a", "session:run-1", "before").status_code, 409);
    EXPECT_EQ(read_sidebar_layout(std::filesystem::path(SessionStorage::get_project_dir(cwd)) / "sidebar_sessions.json"), before);
    auto meta_path = SessionStorage::meta_path(SessionStorage::get_project_dir(cwd), "receiver");
    auto meta = SessionStorage::read_meta(meta_path); meta.archived = true;
    ASSERT_TRUE(SessionStorage::write_meta(meta_path, meta));
    page = get();
    bool promoted = false;
    for (const auto& row : page["sessions"]) if (row["id"] == "run-1") {
        EXPECT_EQ(row["sidebar_parent_key"], "");
        promoted = true;
    }
    EXPECT_TRUE(promoted);
}

TEST_F(SidebarSessionsHttp, OrdinaryRootsRejectChildrenWithoutChangingSavedLayout) {
    record("run", "daily"); record("normal"); record("other");
    ASSERT_EQ(move("session:other", "session:normal", "before").status_code, 200);
    const auto layout_path = std::filesystem::path(SessionStorage::get_project_dir(cwd)) / "sidebar_sessions.json";
    const auto before = read_sidebar_layout(layout_path);
    for (const auto& source : {"session:other", "session:run"}) {
        const auto response = move(source, "session:normal", "inside");
        EXPECT_EQ(response.status_code, 409) << response.text;
        EXPECT_EQ(json::parse(response.text)["error"], "SIDEBAR_SCHEDULED_PARENT_ONLY");
    }
    EXPECT_EQ(read_sidebar_layout(layout_path), before);
    const auto page = get({{"expanded", "[\"session:normal\"]"}});
    EXPECT_EQ(page["total"], 3);
    for (const auto& row : page["sessions"]) if (!row.value("sidebar_group", false)) EXPECT_EQ(row["sidebar_child_count"], 0);
    ASSERT_EQ(move("session:normal", "loop:daily", "inside").status_code, 200);
    EXPECT_EQ(get()["total"], 2);
}

TEST_F(SidebarSessionsHttp, NoWorkspaceIsIsolatedAndLiveOriginsArePreserved) {
    record("workspace-run", "daily");
    SessionOptions options;
    options.no_workspace = true; options.loop_execution = true; options.loop_id = "detached";
    const auto first = registry->create(options);
    const auto second = registry->create(options);
    ASSERT_NE(first, second);
    const auto marked = cpr::Post(cpr::Url{url("/api/session-batch")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{json{{"action", "unread"}, {"items", json::array({
            {{"session_id", first}, {"no_workspace", true}}
        })}}.dump()});
    ASSERT_EQ(marked.status_code, 200) << marked.text;
    const auto response = cpr::Get(cpr::Url{url("/api/session-sidebar")},
        cpr::Parameters{{"expanded", "[\"loop:detached\"]"}}, cpr::Timeout{3000});
    ASSERT_EQ(response.status_code, 200) << response.text;
    const auto body = json::parse(response.text);
    EXPECT_EQ(body["total"], 1);
    ASSERT_EQ(body["sessions"].size(), 3u);
    for (const auto& row : body["sessions"]) EXPECT_TRUE(row["no_workspace"].get<bool>());
    EXPECT_EQ(body["sessions"][0]["attention_state"], "unread");
    for (const auto& row : body["sessions"]) {
        if (row.value("id", std::string{}) == first) EXPECT_EQ(row["manual_unread"], true);
    }
    const auto collapsed = cpr::Get(cpr::Url{url("/api/session-sidebar")});
    ASSERT_EQ(collapsed.status_code, 200);
    const auto group = json::parse(collapsed.text)["sessions"][0];
    EXPECT_EQ(group["attention_state"], "unread");
    for (const auto& member : group["sidebar_members"]) {
        if (member.value("id", std::string{}) == first) EXPECT_EQ(member["manual_unread"], true);
    }
    EXPECT_EQ(get()["total"], 1);
}

TEST_F(SidebarSessionsHttp, PinnedRepresentativeDoesNotConsumeRootSlotsOrHideHistory) {
    record("run-1", "daily"); record("run-2", "daily"); record("run-3", "daily");
    for (int i = 0; i < 6; ++i) record("normal-" + std::to_string(i));
    const auto pin_path = std::filesystem::path(SessionStorage::get_project_dir(cwd)) / "pinned_sessions.json";
    std::string error;
    ASSERT_TRUE(session_pins::write_pinned_sessions_state(pin_path, {{"run-3"}}, &error)) << error;
    auto page = get({{"limit", "5"}, {"expanded", "[\"loop:daily\"]"}});
    EXPECT_EQ(page["total"], 7);
    EXPECT_EQ(page["visible_roots"], 5);
    EXPECT_EQ(page["sessions"].size(), 8u); // Five roots, two executions, one pin.
    EXPECT_EQ(page["sessions"][0]["sidebar_latest_session_id"], "run-2");
    EXPECT_EQ(page["sessions"][1]["id"], "run-2");
    EXPECT_EQ(page["sessions"][2]["id"], "run-1");
    EXPECT_EQ(page["sessions"].back()["id"], "run-3");
    EXPECT_FALSE(page["sessions"].back().contains("sidebar_parent_key"));
}

TEST_F(SidebarSessionsHttp, SingleExecutionMovesAsALeaf) {
    record("run-1", "daily"); record("receiver", "other");
    EXPECT_EQ(move("loop:daily", "loop:other", "inside").status_code, 409);
    auto response = move("session:run-1", "loop:other", "inside");
    ASSERT_EQ(response.status_code, 200) << response.text;
    const auto moved = json::parse(response.text);
    EXPECT_EQ(moved["total"], 1);
    ASSERT_EQ(moved["sessions"].size(), 3u);
    EXPECT_EQ(moved["sessions"][2]["sidebar_key"], "session:run-1");
    record("run-2", "daily");
    auto page = get({{"expanded", "[\"loop:other\"]"}});
    EXPECT_EQ(page["total"], 2);
    ASSERT_EQ(page["sessions"].size(), 4u);
    EXPECT_EQ(page["sessions"][0]["sidebar_latest_session_id"], "run-2");
}

TEST_F(SidebarSessionsHttp, GroupUsesTaskNameLatestTimeAndUnreadAcrossAllMembers) {
    loop::LoopDefinition definition;
    definition.id = "daily";
    definition.name = "Daily briefing";
    definition.prompt = "Inspect only";
    definition.model_name = "fixture";
    definition.workspace_cwd = cwd;
    definition.workspace_hash = compute_cwd_hash(cwd);
    definition.schedule.kind = loop::ScheduleKind::Once;
    definition.schedule.once_at_ms = 10'000;
    const auto task = loops.create_loop(definition, 1'000);
    ASSERT_TRUE(task.has_value());
    record("run-1", "daily"); record("run-2", "daily"); record("normal");
    const auto directory = std::filesystem::path(SessionStorage::get_project_dir(cwd));
    {
        std::ofstream out(directory / "session_read_state.json", std::ios::binary);
        out << json{{"sessions", {{"run-1", {{"read_cursor", 1}, {"update_cursor", 2}}}}}}.dump();
    }
    auto old = SessionStorage::read_meta(SessionStorage::meta_path(directory.string(), "run-1"));
    old.title = "Changed old conversation"; old.updated_at = "zzzz";
    ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(directory.string(), "run-1"), old));
    auto page = get();
    const auto header = page["sessions"][0];
    EXPECT_EQ(header["title"], "Daily briefing");
    EXPECT_EQ(header["sidebar_latest_session_id"], "run-2");
    EXPECT_EQ(header["updated_at"], "run-2");
    EXPECT_EQ(header["attention_state"], "unread");
    EXPECT_FALSE(header["busy"].get<bool>());
    EXPECT_FALSE(header["active"].get<bool>());
    EXPECT_EQ(header["sidebar_members"].size(), 2u);
    auto renamed = *task; renamed.name = "Renamed task";
    ASSERT_TRUE(loops.update_loop(task->id, renamed, 2'000).has_value());
    EXPECT_EQ(get()["sessions"][0]["title"], "Renamed task");
    const auto response = move("session:run-2", "session:normal", "before");
    ASSERT_EQ(response.status_code, 200) << response.text;
    page = get({{"expanded", "[\"loop:daily\"]"}});
    for (const auto& row : page["sessions"]) if (row.value("sidebar_group", false)) {
        EXPECT_EQ(row["id"], header["id"]);
        EXPECT_EQ(row["title"], "Renamed task");
        EXPECT_EQ(row["sidebar_latest_session_id"], "run-1");
    }
}
} // namespace
