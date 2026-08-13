#include <gtest/gtest.h>

#include "config/saved_models.hpp"
#include "provider/anthropic_provider.hpp"
#include "provider/copilot_provider.hpp"
#include "provider/openai_provider.hpp"
#include "provider/provider_factory.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using acecode::AnthropicProvider;
using acecode::ChatMessage;
using acecode::ModelProfile;
using acecode::ModelReasoningOptions;
using acecode::OpenAiCompatProvider;
using acecode::ProviderRequestOptions;
using acecode::ReasoningWireProtocol;
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolDef;

class TestableOpenAiProvider : public OpenAiCompatProvider {
public:
    using OpenAiCompatProvider::OpenAiCompatProvider;
    using OpenAiCompatProvider::build_request_body;
};

class TestableCopilotProvider : public acecode::CopilotProvider {
public:
    using CopilotProvider::CopilotProvider;
    using OpenAiCompatProvider::build_request_body;
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

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

ChatMessage user_message() {
    ChatMessage message;
    message.role = "user";
    message.content = "hello";
    return message;
}

ToolDef test_tool() {
    ToolDef tool;
    tool.name = "file_read";
    tool.description = "Read one file";
    tool.parameters = {
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}},
    };
    return tool;
}

ModelReasoningOptions reasoning_with_effort(const std::string& effort) {
    ModelReasoningOptions reasoning;
    reasoning.supported = true;
    reasoning.default_enabled = true;
    reasoning.enabled = true;
    reasoning.supported_efforts = {"low", "medium", "high", "max"};
    reasoning.default_effort = "medium";
    reasoning.effort = effort;
    return reasoning;
}

} // namespace

TEST(ModelProfileRuntimeOptions, LegacyOpenAiRequestBehaviorIsUnchanged) {
    TestableOpenAiProvider provider(
        "https://example.test/v1/", "key", "legacy-model");
    const auto body = provider.build_request_body(
        {user_message()}, {test_tool()}, false);
    const auto stream_body = provider.build_request_body(
        {user_message()}, {test_tool()}, true);

    EXPECT_EQ(provider.request_url(),
              "https://example.test/v1/chat/completions");
    EXPECT_FALSE(body.contains("max_tokens"));
    EXPECT_FALSE(body.contains("reasoning"));
    ASSERT_TRUE(body.contains("tools"));
    EXPECT_EQ(body["tools"].size(), 1u);
    EXPECT_TRUE(stream_body["stream"].get<bool>());
    EXPECT_FALSE(stream_body.contains("max_tokens"));
    EXPECT_FALSE(stream_body.contains("reasoning"));
    EXPECT_TRUE(stream_body.contains("tools"));
}

TEST(ModelProfileRuntimeOptions, OpenRouterMapsEffortAndOutputForBothModes) {
    ProviderRequestOptions options;
    options.max_output_tokens = 65536;
    options.reasoning = reasoning_with_effort("high");
    options.reasoning_protocol = ReasoningWireProtocol::OpenRouter;
    TestableOpenAiProvider provider(
        "https://openrouter.ai/api/v1", "key", "model", 1000, {}, options);

    for (bool stream : {false, true}) {
        const auto body = provider.build_request_body(
            {user_message()}, {test_tool()}, stream);
        EXPECT_EQ(body["max_tokens"], 65536);
        ASSERT_TRUE(body.contains("reasoning"));
        EXPECT_EQ(body["reasoning"], nlohmann::json({{"effort", "high"}}));
        EXPECT_TRUE(body.contains("tools"));
        EXPECT_EQ(body.contains("stream"), stream);
    }
}

TEST(ModelProfileRuntimeOptions, OpenRouterBudgetWinsAndDisableIsExplicit) {
    ProviderRequestOptions budget_options;
    budget_options.max_output_tokens = 32768;
    auto budget = reasoning_with_effort("high");
    budget.supports_max_tokens = true;
    budget.max_tokens = 8192;
    budget_options.reasoning = budget;
    budget_options.reasoning_protocol = ReasoningWireProtocol::OpenRouter;
    TestableOpenAiProvider budget_provider(
        "https://openrouter.ai/api/v1", "key", "model", 1000, {},
        budget_options);
    auto budget_body = budget_provider.build_request_body(
        {user_message()}, {}, false);
    EXPECT_EQ(budget_body["reasoning"],
              nlohmann::json({{"max_tokens", 8192}}));

    ProviderRequestOptions disabled_options;
    auto disabled = reasoning_with_effort("high");
    disabled.enabled = false;
    disabled_options.reasoning = disabled;
    disabled_options.reasoning_protocol = ReasoningWireProtocol::OpenRouter;
    TestableOpenAiProvider disabled_provider(
        "https://openrouter.ai/api/v1", "key", "model", 1000, {},
        disabled_options);
    auto disabled_body = disabled_provider.build_request_body(
        {user_message()}, {}, true);
    EXPECT_EQ(disabled_body["reasoning"],
              nlohmann::json({{"effort", "none"}}));

    ProviderRequestOptions mandatory_options;
    auto mandatory = reasoning_with_effort("high");
    mandatory.mandatory = true;
    mandatory.enabled = false; // Defensive wire mapping still cannot disable it.
    mandatory_options.reasoning = mandatory;
    mandatory_options.reasoning_protocol = ReasoningWireProtocol::OpenRouter;
    TestableOpenAiProvider mandatory_provider(
        "https://openrouter.ai/api/v1", "key", "model", 1000, {},
        mandatory_options);
    auto mandatory_body = mandatory_provider.build_request_body(
        {user_message()}, {}, false);
    EXPECT_EQ(mandatory_body["reasoning"],
              nlohmann::json({{"effort", "high"}}));
}

TEST(ModelProfileRuntimeOptions, ExplicitToolDisableAppliesAcrossProviders) {
    ProviderRequestOptions disabled;
    disabled.tools_enabled = false;

    TestableOpenAiProvider openai(
        "https://example.test/v1", "key", "model", 1000, {}, disabled);
    EXPECT_FALSE(openai.build_request_body(
        {user_message()}, {test_tool()}, false).contains("tools"));

    AnthropicProvider anthropic(
        AnthropicProvider::kDefaultBaseUrl, "key", "model", 1000, {},
        disabled);
    EXPECT_FALSE(anthropic.build_request_body(
        {user_message()}, {test_tool()}, true).contains("tools"));

    TestableCopilotProvider copilot("model", disabled);
    EXPECT_FALSE(copilot.build_request_body(
        {user_message()}, {test_tool()}, false).contains("tools"));
}

TEST(ModelProfileRuntimeOptions, FullUrlIsExactForStreamingAndNonStreaming) {
    std::mutex mutex;
    std::vector<nlohmann::json> requests;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/custom/chat", [&](const httplib::Request& request,
                                       httplib::Response& response) {
            const auto body = nlohmann::json::parse(request.body);
            {
                std::lock_guard<std::mutex> lock(mutex);
                requests.push_back(body);
            }
            if (body.value("stream", false)) {
                response.set_content(
                    "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
                    "\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n",
                    "text/event-stream");
            } else {
                response.set_content(
                    R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})",
                    "application/json");
            }
        });
    });

    ModelProfile profile;
    profile.name = "exact-endpoint";
    profile.provider = "openai";
    profile.base_url = server.base_url() + "/custom/chat";
    profile.api_key = "test-key";
    profile.model = "test-model";
    profile.endpoint_mode = "full_url";
    profile.max_output_tokens = 12345;
    profile.capabilities_source = "manual";
    profile.capabilities = {};

    auto provider = acecode::create_provider_from_entry(profile);
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->chat({user_message()}, {test_tool()}).content, "ok");

    std::string streamed;
    provider->chat_stream(
        {user_message()}, {test_tool()},
        [&](const StreamEvent& event) {
            if (event.type == StreamEventType::Delta) streamed += event.content;
        });
    EXPECT_EQ(streamed, "ok");

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_FALSE(requests[0].value("stream", false));
    EXPECT_TRUE(requests[1].value("stream", false));
    for (const auto& request : requests) {
        EXPECT_EQ(request["max_tokens"], 12345);
        EXPECT_FALSE(request.contains("tools"));
    }
}

TEST(ModelProfileRuntimeOptions, AnthropicMapsOutputAndThinkingForBothModes) {
    ProviderRequestOptions options;
    options.max_output_tokens = 32768;
    auto reasoning = reasoning_with_effort("high");
    reasoning.supports_max_tokens = true;
    reasoning.max_tokens = 8192;
    options.reasoning = reasoning;
    options.reasoning_protocol = ReasoningWireProtocol::Anthropic;
    AnthropicProvider provider(
        AnthropicProvider::kDefaultBaseUrl, "key", "claude-test", 1000, {},
        options);

    for (bool stream : {false, true}) {
        const auto body = provider.build_request_body(
            {user_message()}, {test_tool()}, stream);
        EXPECT_EQ(body["max_tokens"], 32768);
        EXPECT_EQ(body["thinking"], nlohmann::json({
            {"type", "enabled"}, {"budget_tokens", 8192}}));
        EXPECT_FALSE(body.contains("output_config"));
        EXPECT_TRUE(body.contains("tools"));
        EXPECT_EQ(body.contains("stream"), stream);
    }
}

TEST(ModelProfileRuntimeOptions, AnthropicMapsAdaptiveAndDisabledThinking) {
    ProviderRequestOptions adaptive_options;
    adaptive_options.max_output_tokens = 32768;
    adaptive_options.reasoning = reasoning_with_effort("medium");
    adaptive_options.reasoning_protocol = ReasoningWireProtocol::Anthropic;
    AnthropicProvider adaptive(
        AnthropicProvider::kDefaultBaseUrl, "key", "claude-test", 1000, {},
        adaptive_options);
    auto adaptive_body = adaptive.build_request_body(
        {user_message()}, {}, false);
    EXPECT_EQ(adaptive_body["thinking"],
              nlohmann::json({{"type", "adaptive"}}));
    EXPECT_EQ(adaptive_body["output_config"],
              nlohmann::json({{"effort", "medium"}}));

    ProviderRequestOptions disabled_options;
    auto disabled_reasoning = reasoning_with_effort("high");
    disabled_reasoning.enabled = false;
    disabled_options.reasoning = disabled_reasoning;
    disabled_options.reasoning_protocol = ReasoningWireProtocol::Anthropic;
    AnthropicProvider disabled(
        AnthropicProvider::kDefaultBaseUrl, "key", "claude-test", 1000, {},
        disabled_options);
    auto disabled_body = disabled.build_request_body(
        {user_message()}, {}, true);
    EXPECT_EQ(disabled_body["thinking"],
              nlohmann::json({{"type", "disabled"}}));
    EXPECT_FALSE(disabled_body.contains("output_config"));
}

TEST(ModelProfileRuntimeOptions, AnthropicOmitsUnsafeDefaultAndRejectsExhaustedBudget) {
    ModelProfile profile;
    profile.name = "claude-budget";
    profile.provider = "anthropic";
    profile.base_url = AnthropicProvider::kDefaultBaseUrl;
    profile.api_key = "test-key";
    profile.model = "claude-test";
    profile.max_output_tokens = 8192;
    auto reasoning = reasoning_with_effort("high");
    reasoning.supports_max_tokens = true;
    profile.reasoning = reasoning;

    std::string error;
    EXPECT_TRUE(acecode::validate_saved_models({profile}, "", error)) << error;
    auto safe_default = acecode::create_provider_from_entry(profile);
    ASSERT_NE(safe_default, nullptr);
    auto safe_anthropic = std::dynamic_pointer_cast<AnthropicProvider>(safe_default);
    ASSERT_NE(safe_anthropic, nullptr);
    const auto safe_body = safe_anthropic->build_request_body(
        {user_message()}, {}, false);
    EXPECT_FALSE(safe_body.contains("thinking"));
    EXPECT_EQ(safe_body["output_config"],
              nlohmann::json({{"effort", "high"}}));

    reasoning.effort.reset();
    profile.reasoning = reasoning;
    auto provider_default = acecode::create_provider_from_entry(profile);
    ASSERT_NE(provider_default, nullptr);
    auto default_anthropic =
        std::dynamic_pointer_cast<AnthropicProvider>(provider_default);
    ASSERT_NE(default_anthropic, nullptr);
    const auto default_body = default_anthropic->build_request_body(
        {user_message()}, {}, false);
    EXPECT_FALSE(default_body.contains("thinking"));
    EXPECT_FALSE(default_body.contains("output_config"));

    reasoning.effort = "high";
    reasoning.max_tokens = 8192;
    profile.reasoning = reasoning;
    error.clear();
    EXPECT_FALSE(acecode::validate_saved_models({profile}, "", error));
    EXPECT_NE(error.find("greater than reasoning max_tokens"),
              std::string::npos);
    EXPECT_EQ(acecode::create_provider_from_entry(profile), nullptr);

    reasoning.max_tokens = 4096;
    profile.reasoning = reasoning;
    error.clear();
    EXPECT_TRUE(acecode::validate_saved_models({profile}, "", error)) << error;
    EXPECT_NE(acecode::create_provider_from_entry(profile), nullptr);
}

TEST(ModelProfileRuntimeOptions, SameProviderReconfigureReplacesRuntimeOptions) {
    TestableOpenAiProvider openai(
        "https://old.test/v1", "old", "model");
    ProviderRequestOptions openai_options;
    openai_options.endpoint_mode = "full_url";
    openai_options.max_output_tokens = 6000;
    openai_options.tools_enabled = false;
    openai.reconfigure(" https://new.test/exact/chat ", "new", 2000, {},
                       openai_options);
    EXPECT_EQ(openai.request_url(), "https://new.test/exact/chat");
    const auto openai_body = openai.build_request_body(
        {user_message()}, {test_tool()}, false);
    EXPECT_EQ(openai_body["max_tokens"], 6000);
    EXPECT_FALSE(openai_body.contains("tools"));

    AnthropicProvider anthropic(
        AnthropicProvider::kDefaultBaseUrl, "old", "model");
    ProviderRequestOptions anthropic_options;
    anthropic_options.max_output_tokens = 7000;
    anthropic_options.tools_enabled = false;
    anthropic.reconfigure("https://new-anthropic.test/v1/", "new", 2000, {},
                          anthropic_options);
    const auto anthropic_body = anthropic.build_request_body(
        {user_message()}, {test_tool()}, false);
    EXPECT_EQ(anthropic_body["max_tokens"], 7000);
    EXPECT_FALSE(anthropic_body.contains("tools"));
}

TEST(ModelProfileRuntimeOptions, CopilotRemainsManagedAndRejectsCustomRuntime) {
    ModelProfile valid;
    valid.name = "managed-copilot";
    valid.provider = "copilot";
    valid.model = "gpt-managed";
    auto provider = acecode::create_provider_from_entry(valid);
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->name(), "copilot");
    EXPECT_EQ(provider->model(), "gpt-managed");

    auto custom_endpoint = valid;
    custom_endpoint.base_url = "https://example.test/v1";
    EXPECT_EQ(acecode::create_provider_from_entry(custom_endpoint), nullptr);

    auto custom_key = valid;
    custom_key.api_key = "must-not-be-used";
    EXPECT_EQ(acecode::create_provider_from_entry(custom_key), nullptr);

    auto full_url = valid;
    full_url.endpoint_mode = "full_url";
    EXPECT_EQ(acecode::create_provider_from_entry(full_url), nullptr);

    auto output = valid;
    output.max_output_tokens = 4096;
    EXPECT_EQ(acecode::create_provider_from_entry(output), nullptr);

    auto reasoning = valid;
    reasoning.reasoning = reasoning_with_effort("high");
    EXPECT_EQ(acecode::create_provider_from_entry(reasoning), nullptr);
}

TEST(ModelProfileRuntimeOptions, GrokRemainsManagedAndRejectsCustomRuntime) {
    ModelProfile valid;
    valid.name = "managed-grok";
    valid.provider = "grok";
    valid.model = "grok-4.5";
    auto provider = acecode::create_provider_from_entry(valid);
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->name(), "grok");
    EXPECT_EQ(provider->model(), "grok-4.5");

    auto custom_endpoint = valid;
    custom_endpoint.base_url = "https://example.test/v1";
    EXPECT_EQ(acecode::create_provider_from_entry(custom_endpoint), nullptr);

    auto custom_key = valid;
    custom_key.api_key = "must-not-be-used";
    EXPECT_EQ(acecode::create_provider_from_entry(custom_key), nullptr);

    auto headers = valid;
    headers.request_headers = {{"X-Custom", "forbidden"}};
    EXPECT_EQ(acecode::create_provider_from_entry(headers), nullptr);

    auto timeout = valid;
    timeout.stream_timeout_ms = 30000;
    EXPECT_EQ(acecode::create_provider_from_entry(timeout), nullptr);

    auto output = valid;
    output.max_output_tokens = 4096;
    EXPECT_EQ(acecode::create_provider_from_entry(output), nullptr);
}
