#include <gtest/gtest.h>
#include <cpr/cpr.h>
#include "web/server.hpp"
#include "experts/expert_registry.hpp"
#include "desktop/daemon_supervisor.hpp"
#include "workspace/workspace_registry.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "utils/joining_thread.hpp"
#include "utils/cwd_hash.hpp"
#include "utils/paths.hpp"
#include "session/session_storage.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace acecode;
using nlohmann::json;
using namespace std::chrono_literals;
namespace {
class DesktopOfficeHttp : public testing::Test {
protected:
    acecode_test::MemoryTestHome home{"office-http"};
    std::string cwd = home.workspace_cwd("main");
    std::string projects = (home.root() / ".acecode" / "projects").string();
    AppConfig cfg;
    ToolExecutor tools;
    desktop::WorkspaceRegistry workspaces;
    ExpertRegistry experts{home.root() / ".acecode" / "experts"};
    std::unique_ptr<SessionRegistry> registry;
    std::unique_ptr<LocalSessionClient> client;
    std::unique_ptr<web::WebServer> server;
    JoiningThread worker; // Joined before the server and its dependencies disappear.

    void SetUp() override {
        cfg.web.bind = "127.0.0.1";
        cfg.web.port = desktop::pick_free_loopback_port();
        workspaces.register_new(projects, cwd);
        SessionRegistryDeps sessions;
        sessions.cwd = cwd; sessions.tools = &tools;
        sessions.expert_registry = &experts;
        // Shared with the registry's provider accessor for the fixture lifetime.
        auto provider = std::make_shared<acecode_test::StubLlmProvider>();
        provider->push_text("complete");
        sessions.provider_accessor = [provider] { return provider; };
        registry = std::make_unique<SessionRegistry>(std::move(sessions));
        client = std::make_unique<LocalSessionClient>(*registry);
        web::WebServerDeps deps;
        deps.web_cfg = &cfg.web; deps.app_config = &cfg; deps.cwd = cwd;
        deps.config_path = (home.root() / "config.json").string();
        deps.sync_model_reasoning = false; deps.token = "office-fixture";
        deps.projects_dir = projects; deps.workspace_registry = &workspaces;
        deps.session_client = client.get(); deps.session_registry = registry.get();
        deps.expert_registry = &experts;
        server = std::make_unique<web::WebServer>(std::move(deps));
        worker = JoiningThread([this] { server->run(); });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cpr::Get(cpr::Url{url("/api/health")}, cpr::Timeout{200}).status_code == 200) return;
            std::this_thread::sleep_for(10ms);
        }
        FAIL() << "office HTTP server did not start";
    }
    void TearDown() override {
        if (server) server->stop();
        worker.join(); server.reset(); client.reset(); registry.reset();
    }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(cfg.web.port) + path;
    }
    json snapshot(const std::string& id = {}) {
        json result;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
            const auto response = cpr::Get(cpr::Url{url("/api/desktop-office")},
                cpr::Parameters{{"session", id}}, cpr::Timeout{2000});
            EXPECT_EQ(response.status_code, 200) << response.text;
            if (response.status_code != 200) return {};
            result = json::parse(response.text);
            if (result.value("complete", false)) return result;
            std::this_thread::sleep_for(10ms);
        } while (std::chrono::steady_clock::now() < deadline);
        ADD_FAILURE() << "office catalog did not settle";
        return result;
    }
    std::string active(const std::string& parent = {}, bool no_workspace = false,
                       const std::string& expert = {}, const std::string& member = {}) {
        SessionOptions opts;
        opts.cwd = cwd; opts.parent_session_id = parent; opts.no_workspace = no_workspace;
        opts.expert_id = expert; opts.expert_member_id = member;
        if (!parent.empty()) opts.subagent_depth = 1;
        const auto id = registry->create(opts);
        auto entry = registry->acquire(id);
        ChatMessage message; message.role = "user"; message.content = "test input";
        message.timestamp = "2026-10-06T10:00:00Z";
        entry->sm->on_message(message);
        return id;
    }
};
} // namespace

TEST_F(DesktopOfficeHttp, EmptyAndAuthenticationAndMissingSession) {
    const auto empty = snapshot();
    ASSERT_TRUE(empty.contains("offices"));
    EXPECT_TRUE(empty["offices"].empty());
    EXPECT_TRUE(empty["selected"].is_null());
    EXPECT_TRUE(empty["agents"].empty());
    const auto denied = cpr::Get(cpr::Url{url("/api/desktop-office")},
        cpr::Header{{"Origin", "https://untrusted.invalid"}});
    EXPECT_NE(denied.status_code, 200);
    const auto missing = cpr::Get(cpr::Url{url("/api/desktop-office?session=missing")});
    EXPECT_EQ(missing.status_code, 404);
    EXPECT_EQ(cpr::Options(cpr::Url{url("/api/desktop-office")}).status_code, 204);
}

TEST_F(DesktopOfficeHttp, SnapshotCarriesRealRuntimeAndRoutesMeshChildToRoot) {
    const auto root = active();
    const auto child = active(root);
    auto entry = registry->acquire(root);
    auto worker_entry = registry->acquire(child);
    entry->loop->events().emit(SessionEventKind::BusyChanged, {{"busy", true}, {"turn_id", "root-turn"}});
    worker_entry->loop->events().emit(SessionEventKind::BusyChanged, {{"busy", true}});
    worker_entry->loop->events().emit(SessionEventKind::ToolStart, {{"tool", "file_read"}, {"tool_call_id", "read-1"}});
    worker_entry->loop->events().emit(SessionEventKind::PermissionRequest, {{"request_id", "permission-1"}});
    auto result = snapshot(child);
    ASSERT_EQ(result["selected"]["id"], root);
    ASSERT_EQ(result["offices"].size(), 1u);
    ASSERT_EQ(result["agents"].size(), 2u);
    EXPECT_EQ(result["agents"][1]["activity"]["phase"], "permission_waiting");
    EXPECT_EQ(result["agents"][1]["activity"]["tool"], "file_read");
    entry->loop->events().emit(SessionEventKind::Done, {{"outcome", "completed"}});
    worker_entry->loop->events().emit(SessionEventKind::Done, {{"outcome", "aborted"}});
    result = snapshot(root);
    EXPECT_EQ(result["selected"]["activity"]["outcome"], "completed");
    EXPECT_EQ(result["agents"][1]["activity"]["outcome"], "aborted");
    EXPECT_FALSE(result["selected"].contains("input_draft"));
    EXPECT_FALSE(result["selected"].contains("session_path"));
}

TEST_F(DesktopOfficeHttp, ExpertMemberIdentitySurvivesOfficeProjection) {
    const auto experts_root = home.root() / ".acecode" / "experts";
    const auto write = [](const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << text;
    };
    for (const auto* id : {"leader", "worker"}) {
        write(experts_root / id / "agents" / "lead.md", "# Expert\nHelp the user.\n");
        write(experts_root / id / "expert.json", json({{"name", id}, {"version", "1.0.0"},
            {"expertType", "agent"}, {"displayName", id}, {"agentName", "lead"},
            {"agents", json::array({{{"id", "lead"}, {"path", "agents/lead.md"}}})}}).dump());
    }
    write(experts_root / "team" / "expert.json", json({{"name", "team"}, {"version", "1.0.0"},
        {"expertType", "team"}, {"displayName", "Team"},
        {"teamInfo", {{"leadExpert", "leader"}, {"memberExperts", json::array({"worker"})}}}}).dump());
    ASSERT_TRUE(experts.find(cwd, "team").has_value());
    const auto root = active({}, false, "team");
    const auto child = active(root, false, "team", "worker");
    const auto result = snapshot(root);
    ASSERT_EQ(result["agents"].size(), 2u);
    EXPECT_EQ(result["selected"]["expert"]["avatar_identity"], "expert:leader");
    EXPECT_EQ(result["agents"][1]["id"], child);
    EXPECT_EQ(result["agents"][1]["expert_id"], "team");
    EXPECT_EQ(result["agents"][1]["expert_member_id"], "worker");
    EXPECT_EQ(result["agents"][1]["expert"]["avatar_identity"], "expert:worker");
    const auto catalog = cpr::Get(cpr::Url{url("/api/experts")}, cpr::Timeout{2000});
    ASSERT_EQ(catalog.status_code, 200);
    bool found = false;
    const auto catalog_body = json::parse(catalog.text);
    for (const auto& expert : catalog_body["experts"]) {
        if (expert["id"] != "worker") continue;
        found = true;
        EXPECT_EQ(expert["avatar_identity"], result["agents"][1]["expert"]["avatar_identity"]);
    }
    EXPECT_TRUE(found);
}

TEST_F(DesktopOfficeHttp, HiddenWorkspaceAndNoWorkspaceAreSelectable) {
    const auto hidden_cwd = home.workspace_cwd("not-in-sidebar");
    const auto directory = SessionStorage::get_project_dir(hidden_cwd);
    std::filesystem::create_directories(directory);
    SessionMeta meta;
    meta.id = "20261006-110000-aabb"; meta.cwd = hidden_cwd; meta.title = "Hidden office";
    meta.created_at = "2026-10-06T11:00:00Z"; meta.updated_at = meta.created_at;
    meta.last_user_message_at = meta.created_at; meta.last_turn_outcome = "completed";
    meta.last_token_usage.prompt_tokens = 1200; meta.last_token_usage.has_data = true;
    ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(directory, meta.id), meta));
    auto result = snapshot();
    ASSERT_EQ(result["offices"].size(), 1u);
    EXPECT_EQ(result["selected"]["id"], meta.id);
    EXPECT_EQ(result["selected"]["token_usage"]["prompt_tokens"], 1200);
    EXPECT_EQ(result["selected"]["last_turn_outcome"], "completed");
    const auto unbound = active({}, true);
    result = snapshot(unbound);
    EXPECT_EQ(result["selected"]["id"], unbound);
    EXPECT_EQ(result["selected"]["workspace_hash"], "");
    EXPECT_TRUE(result["selected"]["no_workspace"].get<bool>());
}

// 场景:同一工作区有三个已装载会话——结果未读的主会话、已读的主会话、结果未读的子会话。
// 期望:activities(陪伴宠物的会话卡片来源,复刻 Codex Mini)只列出未读的主会话,
// 带 attention_state=unread 与 activity;已读会话、子会话都不出现(子会话随主会话)。
// 回复只写在会话文件里(daemon 重启后内存里没有)时,卡片仍从文件倒读出回复开头。
TEST_F(DesktopOfficeHttp, ActivitiesListOnlyUnreadOrRunningMainSessions) {
    const auto unread = active();
    const auto read = active();
    const auto child = active(unread);
    ChatMessage reply; reply.role = "assistant"; reply.content = "30 秒已过。";
    registry->acquire(unread)->sm->on_message(reply);
    const auto dir = std::filesystem::path(SessionStorage::get_project_dir(cwd));
    std::filesystem::create_directories(dir);
    const json record_unread = {{"read_cursor", 0}, {"update_cursor", 5}, {"updated_at_ms", 1}};
    const json record_read = {{"read_cursor", 5}, {"update_cursor", 5}, {"updated_at_ms", 1}};
    std::ofstream(dir / "session_read_state.json") << json{{"sessions",
        {{unread, record_unread}, {read, record_read}, {child, record_unread}}}}.dump();
    const auto result = snapshot();
    ASSERT_TRUE(result.contains("activities") && result["activities"].is_array()) << result.dump();
    ASSERT_EQ(result["activities"].size(), 1u) << result["activities"].dump();
    const auto& card = result["activities"][0];
    EXPECT_EQ(card["id"], unread);
    EXPECT_EQ(card["attention_state"], "unread");
    ASSERT_TRUE(card.contains("activity"));
    EXPECT_EQ(card["activity"].value("reply", std::string{}), "30 秒已过。");
    EXPECT_FALSE(card.contains("cwd")) << "cards carry display fields only";
}
