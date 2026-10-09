#include <gtest/gtest.h>

#include "web/handlers/model_connection_test_handler.hpp"
#include "web/handlers/models_handler.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace {

using nlohmann::json;
using acecode::web::test_model_connection;
using namespace std::chrono_literals;

struct ModelTestServer {
    httplib::Server server;
    int port = 0;
    std::thread thread;

    explicit ModelTestServer(const std::function<void(httplib::Server&)>& setup) {
        setup(server);
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 100 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }
    ~ModelTestServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

json request_for(const ModelTestServer& server) {
    return {{"provider", "openai"}, {"model", "test-model"},
            {"api_key", "test-key"}, {"base_url", server.url()}};
}

void reply_ok(httplib::Response& response) {
    response.set_content(
        R"({"choices":[{"message":{"content":"OK"},"finish_reason":"stop"}]})",
        "application/json");
}

acecode::AppConfig saved_config(const ModelTestServer& server) {
    acecode::AppConfig config;
    acecode::ModelProfile profile;
    profile.name = "saved";
    profile.provider = "openai";
    profile.model = "original-model";
    profile.base_url = server.url();
    profile.api_key = "saved-key";
    config.saved_models.push_back(profile);
    config.default_model_name = profile.name;
    return config;
}

TEST(ModelConnectionTest, SendsOnlyOneShortPromptAndPreservesRequestOptions) {
    std::atomic<int> requests{0};
    ModelTestServer upstream([&](httplib::Server& server) {
        server.Post("/custom/chat", [&](const httplib::Request& request,
                                         httplib::Response& response) {
            ++requests;
            const auto body = json::parse(request.body);
            EXPECT_EQ(body["model"], "test-model");
            EXPECT_EQ(body["messages"], (json::array({
                {{"role", "user"}, {"content", "Reply with OK."}},
            })));
            EXPECT_FALSE(body.contains("tools"));
            EXPECT_EQ(body["max_tokens"], 37);
            EXPECT_EQ(request.get_header_value("Authorization"), "Bearer test-key");
            EXPECT_EQ(request.get_header_value("X-Test"), "custom-header");
            reply_ok(response);
        });
    });
    auto input = request_for(upstream);
    input["name"] = "saved";
    input["base_url"] = upstream.url() + "/custom/chat";
    input["endpoint_mode"] = "full_url";
    input["request_headers"] = {{"X-Test", "custom-header"}};
    input["max_output_tokens"] = 37;
    const auto config = saved_config(upstream);
    const auto before = acecode::web::list_models(config);
    const auto result = test_model_connection(input, config);
    EXPECT_EQ(result.status, 200) << result.body;
    EXPECT_EQ(result.body, (json{{"ok", true}}));
    EXPECT_EQ(requests.load(), 1);
    EXPECT_EQ(acecode::web::list_models(config), before);
    EXPECT_EQ(config.default_model_name, "saved");
}

TEST(ModelConnectionTest, AceModelDefaultAndExplicitProtocolUseMatchingEndpoints) {
    std::atomic<int> responses_requests{0};
    std::atomic<int> chat_requests{0};
    ModelTestServer upstream([&](httplib::Server& server) {
        server.Post("/responses", [&](const httplib::Request& request, httplib::Response& response) {
            ++responses_requests;
            const auto body = json::parse(request.body);
            EXPECT_TRUE(body.contains("input"));
            EXPECT_FALSE(body.contains("messages"));
            response.set_content(R"({"status":"completed","output":[{"type":"message",
                "role":"assistant","content":[{"type":"output_text","text":"OK"}]}]})",
                "application/json");
        });
        server.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& response) {
            ++chat_requests;
            reply_ok(response);
        });
    });
    auto input = request_for(upstream);
    input["models_dev_provider_id"] = "acemodel";
    const auto config = saved_config(upstream);
    EXPECT_EQ(test_model_connection(input, config).status, 200);
    input["api_protocol"] = "chat_completions";
    EXPECT_EQ(test_model_connection(input, config).status, 200);
    input["api_protocol"] = "responses";
    EXPECT_EQ(test_model_connection(input, config).status, 200);
    EXPECT_EQ(responses_requests.load(), 2);
    EXPECT_EQ(chat_requests.load(), 1);
    EXPECT_FALSE(config.saved_models[0].api_protocol.has_value());
}

TEST(ModelConnectionTest, ResolvesCredentialReuseAndEditingWithoutMutation) {
    std::atomic<int> requests{0};
    ModelTestServer upstream([&](httplib::Server& server) {
        server.Post("/chat/completions", [&](const httplib::Request& request,
                                              httplib::Response& response) {
            ++requests;
            EXPECT_EQ(request.get_header_value("Authorization"), "Bearer saved-key");
            EXPECT_EQ(json::parse(request.body)["model"], "test-model");
            reply_ok(response);
        });
    });
    const auto config = saved_config(upstream);
    auto input = request_for(upstream);
    input.erase("api_key");
    input["credential_source_name"] = "saved";
    EXPECT_EQ(test_model_connection(input, config).status, 200);
    input.erase("credential_source_name");
    input["original_name"] = "saved";
    EXPECT_EQ(test_model_connection(input, config).status, 200);
    EXPECT_EQ(requests.load(), 2);
    EXPECT_EQ(config.saved_models.front().model, "original-model");
    EXPECT_EQ(config.saved_models.front().api_key, "saved-key");
}

TEST(ModelConnectionTest, RejectsInvalidCredentialSourceWithoutUpstreamRequest) {
    std::atomic<int> requests{0};
    ModelTestServer upstream([&](httplib::Server& server) {
        server.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& response) {
            ++requests;
            reply_ok(response);
        });
    });
    auto input = request_for(upstream);
    input.erase("api_key");
    input["credential_source_name"] = "missing";
    auto result = test_model_connection(input, saved_config(upstream));
    EXPECT_EQ(result.status, 400);
    EXPECT_EQ(result.body["error"], "INVALID_CREDENTIAL_SOURCE");
    input["credential_source_name"] = "saved";
    input["base_url"] = upstream.url() + "/different";
    result = test_model_connection(input, saved_config(upstream));
    EXPECT_EQ(result.body["error"], "INVALID_CREDENTIAL_SOURCE");
    EXPECT_EQ(requests.load(), 0);
}

TEST(ModelConnectionTest, RejectsEmptyReasoningOnlyToolOnlyAndMalformedReplies) {
    const std::vector<std::string> replies = {
        R"({"choices":[{"message":{"content":" \n\t"}}]})",
        R"({"choices":[{"message":{"content":"","reasoning_content":"thinking"}}]})",
        R"({"choices":[{"message":{"tool_calls":[{"id":"call1","type":"function","function":{"name":"example","arguments":"{}"}}]}}]})",
        "{}", "invalid json",
    };
    for (const auto& reply : replies) {
        ModelTestServer upstream([&](httplib::Server& server) {
            server.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& response) {
                response.set_content(reply, "application/json");
            });
        });
        const auto result = test_model_connection(request_for(upstream), {});
        EXPECT_EQ(result.status, 502) << reply << result.body;
        EXPECT_FALSE(result.body.contains("ok"));
    }
}

TEST(ModelConnectionTest, RedactsUpstreamFailureAndDoesNotRetry) {
    std::atomic<int> requests{0};
    ModelTestServer upstream([&](httplib::Server& server) {
        server.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& response) {
            ++requests;
            response.status = 429;
            response.set_content(R"({"error":{"message":"secret test-key saved-key"}})", "application/json");
        });
    });
    const auto result = test_model_connection(request_for(upstream), {});
    EXPECT_EQ(result.status, 502);
    EXPECT_EQ(result.body["error"], "MODEL_TEST_HTTP_ERROR");
    EXPECT_EQ(result.body["upstream_status"], 429);
    EXPECT_EQ(result.body.dump().find("secret"), std::string::npos);
    EXPECT_EQ(result.body.dump().find("test-key"), std::string::npos);
    EXPECT_EQ(requests.load(), 1);
}

TEST(ModelConnectionTest, UsesAnthropicProtocol) {
    ModelTestServer upstream([](httplib::Server& server) {
        server.Post("/v1/messages", [](const httplib::Request& request, httplib::Response& response) {
            EXPECT_EQ(request.get_header_value("x-api-key"), "test-key");
            const auto body = json::parse(request.body);
            EXPECT_EQ(body["messages"][0]["content"],
                      (json::array({{{"type", "text"}, {"text", "Reply with OK."}}})));
            EXPECT_FALSE(body.contains("tools"));
            response.set_content(R"({"type":"message","content":[{"type":"text","text":"OK"}],"stop_reason":"end_turn"})", "application/json");
        });
    });
    auto input = request_for(upstream);
    input["provider"] = "anthropic";
    input["base_url"] = upstream.url() + "/v1";
    const auto result = test_model_connection(input, {});
    EXPECT_EQ(result.status, 200) << result.body;
}

TEST(ModelConnectionTest, ReportsTimeoutAndInvalidDrafts) {
    ModelTestServer upstream([](httplib::Server& server) {
        server.Post("/chat/completions", [](const httplib::Request&, httplib::Response& response) {
            std::this_thread::sleep_for(200ms);
            reply_ok(response);
        });
    });
    auto input = request_for(upstream);
    input["stream_timeout_ms"] = 50;
    const auto result = test_model_connection(input, {});
    EXPECT_EQ(result.status, 504);
    EXPECT_EQ(result.body["error"], "MODEL_TEST_TIMEOUT");
    EXPECT_EQ(test_model_connection(json::array(), {}).status, 400);
    input["original_name"] = 7;
    EXPECT_EQ(test_model_connection(input, {}).status, 400);
    input.erase("original_name");
    input.erase("model");
    EXPECT_EQ(test_model_connection(input, {}).status, 400);
}

} // namespace
