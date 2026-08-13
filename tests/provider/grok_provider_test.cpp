#include "provider/grok_provider.hpp"
#include "provider/auth/xai_auth.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path root;
    explicit TempDir(const std::string& name) {
        root = fs::temp_directory_path() /
            (name + "_" + std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

struct LocalHttpServer {
    httplib::Server server;
    int port = 0;
    std::thread thread;
    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(server);
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 50 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }
    ~LocalHttpServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
    std::string base_url(const std::string& path = "") const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

int64_t future_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600;
}

acecode::ChatMessage user_message(const std::string& text = "hello") {
    acecode::ChatMessage message;
    message.role = "user";
    message.content = text;
    return message;
}

acecode::GrokAuthConfig test_config(const LocalHttpServer& upstream,
                                    const TempDir& temp) {
    acecode::GrokAuthConfig config;
    config.build_base_url = upstream.base_url("/v1");
    config.token_url = upstream.base_url("/token");
    config.credential_path = acecode::path_to_utf8(temp.root / "grok_auth.json");
    config.client_version = "test-version";
    config.timeout_ms = 5000;
    return config;
}

void seed_credentials(const acecode::GrokAuthConfig& config,
                      const std::string& access = "access-old",
                      const std::string& refresh = "refresh-old") {
    ASSERT_TRUE(acecode::save_grok_auth_tokens(
        {access, refresh, future_time(), "user-123", "user@example.com"},
        config.credential_path));
}

} // namespace

TEST(GrokProviderTest, SendsResponsesProtocolAndStableOfficialHeaders) {
    TempDir temp("acecode_grok_provider_headers");
    std::mutex mutex;
    std::vector<std::string> agent_ids;
    std::vector<std::string> session_ids;
    std::vector<std::string> request_ids;
    std::vector<std::string> traceparents;
    std::vector<nlohmann::json> bodies;

    LocalHttpServer upstream([&](httplib::Server& server) {
        server.Post("/v1/responses", [&](const httplib::Request& request,
                                         httplib::Response& response) {
            EXPECT_EQ(request.get_header_value("Authorization"),
                      "Bearer access-old");
            EXPECT_EQ(request.get_header_value("X-XAI-Token-Auth"),
                      "xai-grok-cli");
            EXPECT_EQ(request.get_header_value("x-grok-client-version"),
                      "test-version");
            EXPECT_EQ(request.get_header_value("x-grok-client-identifier"),
                      "grok-shell");
            EXPECT_EQ(request.get_header_value("x-grok-client-mode"),
                      "headless");
            EXPECT_EQ(request.get_header_value("x-authenticateresponse"),
                      "authenticate-response");
            EXPECT_EQ(request.get_header_value("x-grok-model-override"),
                      "grok-4.5");
            EXPECT_EQ(request.get_header_value("x-grok-user-id"), "user-123");
            EXPECT_EQ(request.get_header_value("Accept"), "application/json");
            EXPECT_EQ(request.get_header_value("Accept-Encoding"), "gzip");
            EXPECT_EQ(request.get_header_value("User-Agent"),
                      "grok-shell/test-version (linux; x86_64)");
            {
                std::lock_guard<std::mutex> lock(mutex);
                agent_ids.push_back(request.get_header_value("x-grok-agent-id"));
                session_ids.push_back(request.get_header_value("x-grok-session-id"));
                request_ids.push_back(request.get_header_value("x-grok-req-id"));
                traceparents.push_back(request.get_header_value("traceparent"));
                bodies.push_back(nlohmann::json::parse(request.body));
            }
            response.set_content(R"({
                "id":"resp_1","status":"completed",
                "output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}],
                "usage":{"input_tokens":5,"output_tokens":1,"total_tokens":6}
            })", "application/json");
        });
    });
    const auto config = test_config(upstream, temp);
    seed_credentials(config);
    acecode::GrokProvider provider("grok-4.5", {}, config, 5000);

    const auto first = provider.chat({user_message()}, {});
    const auto second = provider.chat({user_message("again")}, {});
    EXPECT_EQ(first.content, "ok");
    EXPECT_EQ(second.content, "ok");
    EXPECT_EQ(first.usage.total_tokens, 6);

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(bodies.size(), 2u);
    EXPECT_EQ(bodies[0]["store"], false);
    EXPECT_EQ(bodies[0]["model"], "grok-4.5");
    EXPECT_EQ(bodies[0]["prompt_cache_key"], session_ids[0]);
    EXPECT_EQ(bodies[0]["input"][0]["type"], "message");
    EXPECT_EQ(agent_ids[0], agent_ids[1]);
    EXPECT_EQ(session_ids[0], session_ids[1]);
    EXPECT_NE(request_ids[0], request_ids[1]);
    EXPECT_FALSE(agent_ids[0].empty());
    EXPECT_FALSE(session_ids[0].empty());
    const std::regex trace_pattern("^00-[0-9a-f]{32}-[0-9a-f]{16}-01$");
    EXPECT_TRUE(std::regex_match(traceparents[0], trace_pattern));
    EXPECT_TRUE(std::regex_match(traceparents[1], trace_pattern));
}

TEST(GrokProviderTest, NonStreamingUnauthorizedRefreshesAndRetriesOnce) {
    TempDir temp("acecode_grok_provider_refresh");
    std::atomic<int> response_requests{0};
    std::atomic<int> refresh_requests{0};
    LocalHttpServer upstream([&](httplib::Server& server) {
        server.Post("/token", [&](const httplib::Request& request,
                                  httplib::Response& response) {
            ++refresh_requests;
            EXPECT_EQ(request.get_param_value("refresh_token"), "refresh-old");
            response.set_content(
                R"({"access_token":"access-new","refresh_token":"refresh-new","expires_in":3600})",
                "application/json");
        });
        server.Post("/v1/responses", [&](const httplib::Request& request,
                                         httplib::Response& response) {
            ++response_requests;
            if (request.get_header_value("Authorization") ==
                "Bearer access-old") {
                response.status = 401;
                response.set_content(
                    R"({"error":{"message":"expired","access_token":"must-not-leak"}})",
                    "application/json");
                return;
            }
            EXPECT_EQ(request.get_header_value("Authorization"),
                      "Bearer access-new");
            response.set_content(
                R"({"status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"recovered"}]}]})",
                "application/json");
        });
    });
    const auto config = test_config(upstream, temp);
    seed_credentials(config);
    acecode::GrokProvider provider("grok-4.5", {}, config, 5000);

    const auto response = provider.chat({user_message()}, {});
    EXPECT_EQ(response.content, "recovered");
    EXPECT_EQ(response_requests.load(), 2);
    EXPECT_EQ(refresh_requests.load(), 1);
    const auto persisted = acecode::load_grok_auth_tokens(config.credential_path);
    EXPECT_EQ(persisted.access_token, "access-new");
    EXPECT_EQ(persisted.refresh_token, "refresh-new");
}

TEST(GrokProviderTest, StreamsResponsesEventsAndPreservesEncryptedReasoning) {
    TempDir temp("acecode_grok_provider_stream");
    LocalHttpServer upstream([&](httplib::Server& server) {
        server.Post("/v1/responses", [&](const httplib::Request& request,
                                         httplib::Response& response) {
            const auto body = nlohmann::json::parse(request.body);
            EXPECT_EQ(body["stream"], true);
            EXPECT_EQ(request.get_header_value("Accept"), "text/event-stream");
            EXPECT_EQ(request.get_header_value("Accept-Encoding"), "identity");
            const auto chunks = std::make_shared<std::vector<std::string>>(
                std::initializer_list<std::string>{
                    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\n\n",
                    "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"plan\"}\n\n",
                    "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"rs_1\",\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"cipher\"}}\n\n",
                    "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"output\":[],\"usage\":{\"input_tokens\":4,\"output_tokens\":2,\"total_tokens\":6}}}\n\n",
                });
            const auto index = std::make_shared<std::size_t>(0);
            response.set_chunked_content_provider(
                "text/event-stream",
                [chunks, index](std::size_t, httplib::DataSink& sink) {
                    if (*index >= chunks->size()) {
                        sink.done();
                        return true;
                    }
                    const std::string& chunk = (*chunks)[(*index)++];
                    sink.write(chunk.data(), chunk.size());
                    return true;
                });
        });
    });
    const auto config = test_config(upstream, temp);
    seed_credentials(config);
    acecode::GrokProvider provider("grok-4.5", {}, config, 5000);
    std::vector<acecode::StreamEvent> events;
    provider.chat_stream(
        {user_message()}, {},
        [&](const acecode::StreamEvent& event) { events.push_back(event); });

    bool saw_text = false;
    bool saw_reasoning = false;
    bool saw_usage = false;
    bool saw_done = false;
    for (const auto& event : events) {
        if (event.type == acecode::StreamEventType::Delta) {
            saw_text = event.content == "hello";
        } else if (event.type == acecode::StreamEventType::ReasoningDelta) {
            saw_reasoning = event.content == "plan";
        } else if (event.type == acecode::StreamEventType::Usage) {
            saw_usage = event.usage.total_tokens == 6;
        } else if (event.type == acecode::StreamEventType::Done) {
            saw_done = event.finish_reason == "stop" &&
                event.content_parts.size() == 1 &&
                event.content_parts[0]["encrypted_content"] == "cipher";
        }
    }
    EXPECT_TRUE(saw_text);
    EXPECT_TRUE(saw_reasoning);
    EXPECT_TRUE(saw_usage);
    EXPECT_TRUE(saw_done);
}

TEST(GrokProviderTest, MissingCredentialsReturnsStructuredManagedAuthError) {
    TempDir temp("acecode_grok_provider_no_auth");
    LocalHttpServer upstream([](httplib::Server&) {});
    const auto config = test_config(upstream, temp);
    acecode::GrokProvider provider("grok-4.5", {}, config, 5000);
    const auto response = provider.chat({user_message()}, {});
    EXPECT_EQ(response.finish_reason, "error");
    EXPECT_EQ(response.provider_error.provider, "grok");
    EXPECT_EQ(response.provider_error.status_code, 401);
    EXPECT_NE(response.provider_error.display_message.find("authentication"),
              std::string::npos);
}
