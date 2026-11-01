#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <asio.hpp>
#include <gtest/gtest.h>
#include <httplib.h>
#include <cpr/cpr.h>

#include "web/server.hpp"
#include "desktop/daemon_supervisor.hpp"
#include "session_host/local_session_client.hpp"
#include "session_host/session_registry.hpp"
#include "session/turn_timing.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "utils/joining_thread.hpp"

using namespace acecode;
using nlohmann::json;
using namespace std::chrono_literals;
namespace {
class FollowUpPromptHttp : public testing::Test {
protected:
    acecode_test::MemoryTestHome home{"follow-up-http"};
    std::string cwd = home.workspace_cwd("main");
    AppConfig config;
    ToolExecutor tools;
    std::unique_ptr<SessionRegistry> registry;
    std::unique_ptr<LocalSessionClient> client;
    std::unique_ptr<web::WebServer> server;
    httplib::Server model;
    json model_request;
    std::function<void()> during_generation;
    std::atomic<int> model_calls{0};
    JoiningThread model_worker;
    JoiningThread worker;

    void SetUp() override {
        model.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
            model_request = json::parse(req.body);
            ++model_calls;
            if (during_generation) during_generation();
            const json message = {{"role", "assistant"}, {"content", R"({"prompt":"Run the parser tests."})"}};
            const json choices = json::array({{{"index", 0}, {"finish_reason", "stop"}, {"message", message}}});
            res.set_content(json{{"id", "follow-up"}, {"object", "chat.completion"},
                {"choices", choices}}.dump(), "application/json");
        });
        const auto port = model.bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        model_worker = JoiningThread([this] { model.listen_after_bind(); });
        ModelProfile profile;
        profile.name = "suggestion"; profile.provider = "openai"; profile.model = "test-model";
        profile.base_url = "http://127.0.0.1:" + std::to_string(port) + "/v1";
        profile.api_key = "fixture-only";
        config.saved_models = {profile};
        config.summary_generation.model_name = profile.name;
        config.default_model_name = profile.name;
        config.ui.locale = "en-US";
        config.web.bind = "127.0.0.1";
        config.web.port = desktop::pick_free_loopback_port();
        SessionRegistryDeps sessions;
        sessions.cwd = cwd; sessions.tools = &tools;
        registry = std::make_unique<SessionRegistry>(std::move(sessions));
        client = std::make_unique<LocalSessionClient>(*registry);
        web::WebServerDeps deps;
        deps.web_cfg = &config.web; deps.app_config = &config; deps.cwd = cwd;
        deps.config_path = (home.root() / "config.json").string();
        deps.sync_model_reasoning = false; deps.token = "follow-up-fixture";
        deps.session_client = client.get(); deps.session_registry = registry.get();
        server = std::make_unique<web::WebServer>(std::move(deps));
        worker = JoiningThread([this] { server->run(); });
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (cpr::Get(cpr::Url{url("/api/health")}, cpr::Timeout{200}).status_code == 200) return;
            std::this_thread::sleep_for(10ms);
        }
        FAIL() << "follow-up fixture did not start";
    }
    void TearDown() override {
        if (server) server->stop();
        worker.join();
        model.stop(); model_worker.join();
        server.reset(); client.reset(); registry.reset();
    }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(config.web.port) + path;
    }
    std::string completed(const std::string& outcome = "completed") {
        SessionOptions options; options.cwd = cwd;
        const auto id = registry->create(options);
        const auto entry = registry->acquire(id);
        ChatMessage user; user.role = "user"; user.uuid = "u1"; user.content = "Implement the parser";
        entry->sm->on_message(user);
        ChatMessage assistant; assistant.role = "assistant"; assistant.content = "Implemented. Tests remain.";
        entry->sm->on_message(assistant);
        entry->sm->on_message(make_turn_timing_message({"u1", 100, 200, 100, outcome}, ""));
        return id;
    }
    cpr::Response suggest(const std::string& id, json body = {{"completion_key", "u1:200"}}) {
        return cpr::Post(cpr::Url{url("/api/sessions/" + id + "/follow-up-prompt")},
            cpr::Header{{"Content-Type", "application/json"}}, cpr::Body{body.dump()}, cpr::Timeout{20000});
    }
};
}

TEST_F(FollowUpPromptHttp, GeneratesWithoutToolsOrTranscriptMutation) {
    const auto id = completed();
    const auto entry = registry->acquire(id);
    const auto before = entry->sm->load_active_messages().size();
    const auto response = suggest(id);
    ASSERT_EQ(response.status_code, 200) << response.text;
    EXPECT_EQ(json::parse(response.text)["prompt"], "Run the parser tests.");
    EXPECT_EQ(model_calls.load(), 1);
    EXPECT_TRUE(!model_request.contains("tools") || model_request["tools"].empty());
    EXPECT_EQ(entry->sm->load_active_messages().size(), before);
    EXPECT_FALSE(entry->loop->is_busy());
}

TEST_F(FollowUpPromptHttp, RejectsInvalidMissingAbortedAndStaleRequests) {
    EXPECT_EQ(suggest("missing").status_code, 404);
    const auto id = completed("aborted");
    EXPECT_EQ(suggest(id).status_code, 409);
    EXPECT_EQ(suggest(id, {{"completion_key", 42}}).status_code, 400);
    EXPECT_EQ(suggest(id, {{"completion_key", "u2:300"}}).status_code, 409);
    const auto denied = cpr::Post(cpr::Url{url("/api/sessions/" + id + "/follow-up-prompt")},
        cpr::Header{{"Origin", "https://untrusted.invalid"}}, cpr::Body{R"({"completion_key":"u1:200"})"});
    EXPECT_NE(denied.status_code, 200);
    EXPECT_EQ(model_calls.load(), 0);
}

TEST_F(FollowUpPromptHttp, DiscardsResultWhenNewUserInputArrives) {
    const auto id = completed();
    const auto entry = registry->acquire(id);
    during_generation = [entry] {
        ChatMessage user; user.role = "user"; user.uuid = "u2"; user.content = "Different task";
        entry->sm->on_message(user);
    };
    EXPECT_EQ(suggest(id).status_code, 409);
    EXPECT_EQ(model_calls.load(), 1);
}
