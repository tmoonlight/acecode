#include <gtest/gtest.h>
#include <cpr/cpr.h>

#include "web/server.hpp"
#include "desktop/daemon_supervisor.hpp"
#include "session/sidebar_tree.hpp"
#include "session/session_pin_store.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include "workspace/workspace_registry.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "utils/cwd_hash.hpp"
#include "utils/joining_thread.hpp"

#include <chrono>
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
    std::unique_ptr<SessionRegistry> registry;
    std::unique_ptr<LocalSessionClient> client;
    std::unique_ptr<web::WebServer> server;
    JoiningThread worker;

    void SetUp() override {
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

TEST_F(SidebarSessionsHttp, PagesGroupsAndReturnsAllExpandedHistory) {
    for (int i = 10; i < 23; ++i) record("run-" + std::to_string(i), "daily");
    for (int i = 0; i < 6; ++i) record("normal-" + std::to_string(i));
    auto page = get({{"limit", "5"}});
    EXPECT_EQ(page["total"], 7);
    EXPECT_EQ(page["sessions"].size(), 5u);
    EXPECT_TRUE(page["has_more"].get<bool>());
    EXPECT_EQ(page["sessions"][0]["id"], "run-22");
    page = get({{"limit", "5"}, {"expanded", "[\"loop:daily\"]"}});
    EXPECT_EQ(page["sessions"].size(), 17u);
    EXPECT_EQ(page["visible_roots"], 5);
    EXPECT_EQ(page["sessions"][12]["id"], "run-10");
    page = get({{"limit", "5"}, {"reveal", "run-10"}});
    EXPECT_EQ(page["sessions"].size(), 17u);
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
    for (const auto& source : {"session:other", "loop:daily"}) {
        const auto response = move(source, "session:normal", "inside");
        EXPECT_EQ(response.status_code, 409) << response.text;
        EXPECT_EQ(json::parse(response.text)["error"], "SIDEBAR_SCHEDULED_PARENT_ONLY");
    }
    EXPECT_EQ(read_sidebar_layout(layout_path), before);
    const auto page = get({{"expanded", "[\"session:normal\"]"}});
    EXPECT_EQ(page["total"], 3);
    for (const auto& row : page["sessions"]) EXPECT_EQ(row["sidebar_child_count"], 0);
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
    const auto response = cpr::Get(cpr::Url{url("/api/session-sidebar")},
        cpr::Parameters{{"expanded", "[\"loop:detached\"]"}}, cpr::Timeout{3000});
    ASSERT_EQ(response.status_code, 200) << response.text;
    const auto body = json::parse(response.text);
    EXPECT_EQ(body["total"], 1);
    ASSERT_EQ(body["sessions"].size(), 2u);
    for (const auto& row : body["sessions"]) EXPECT_TRUE(row["no_workspace"].get<bool>());
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
    EXPECT_EQ(page["sessions"].size(), 7u); // Five roots, one history, one pin.
    EXPECT_EQ(page["sessions"][0]["id"], "run-2");
    EXPECT_EQ(page["sessions"][1]["id"], "run-1");
    EXPECT_EQ(page["sessions"].back()["id"], "run-3");
    EXPECT_FALSE(page["sessions"].back().contains("sidebar_parent_key"));
}

TEST_F(SidebarSessionsHttp, SingleRunGroupMovesAsALeaf) {
    record("run-1", "daily"); record("receiver", "other");
    auto response = move("loop:daily", "loop:other", "inside");
    ASSERT_EQ(response.status_code, 200) << response.text;
    const auto moved = json::parse(response.text);
    EXPECT_EQ(moved["total"], 1);
    ASSERT_EQ(moved["sessions"].size(), 2u);
    EXPECT_EQ(moved["sessions"][1]["sidebar_key"], "session:run-1");
    record("run-2", "daily");
    auto page = get({{"expanded", "[\"loop:other\"]"}});
    EXPECT_EQ(page["total"], 2);
    ASSERT_EQ(page["sessions"].size(), 3u);
    EXPECT_EQ(page["sessions"][0]["id"], "run-2");
}
} // namespace
