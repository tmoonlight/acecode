#include "provider/openai_responses_provider.hpp"
#include "provider/provider_factory.hpp"
#include "utils/joining_thread.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace acecode;
using Json = nlohmann::json;

class LocalHttpServer {
public:
    explicit LocalHttpServer(const std::function<void(httplib::Server&)>& setup) {
        setup(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        // The host stops and joins its listener before server_ is destroyed.
        listener_ = JoiningThread([this] { server_.listen_after_bind(); });
        for (int i = 0; i < 100 && !server_.is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
    }
    ~LocalHttpServer() {
        server_.stop();
        listener_.join();
    }
    std::string url(const std::string& suffix = "/v1") const {
        return "http://127.0.0.1:" + std::to_string(port_) + suffix;
    }

private:
    httplib::Server server_;
    int port_ = 0;
    JoiningThread listener_;
};

// Exercise per-call cache isolation and explicit field-rejection fallback
// without sending a paid request to the official endpoint.
class CacheResponsesProvider : public OpenAiResponsesProvider {
public:
    using OpenAiResponsesProvider::OpenAiResponsesProvider;
protected:
    bool supports_prompt_cache_key() const override { return true; }
};

ChatMessage user_message(const std::string& content = "hello") {
    ChatMessage message;
    message.role = "user";
    message.content = content;
    return message;
}

ToolDef lookup_tool() {
    return {"lookup", "Look up a city", Json{{"type", "object"},
        {"properties", {{"city", {{"type", "string"}}}}}}};
}

Json message_item(const std::string& text = "ok") {
    return {{"id", "msg_1"}, {"type", "message"}, {"status", "completed"},
        {"role", "assistant"}, {"content", Json::array({
            {{"type", "output_text"}, {"text", text}, {"annotations", Json::array()}}
        })}};
}

Json call_item(const std::string& id = "call_1") {
    return {{"id", "fc_1"}, {"type", "function_call"}, {"status", "completed"},
        {"call_id", id}, {"name", "lookup"}, {"arguments", "{\"city\":\"Taipei\"}"}};
}

Json completed(Json output = Json::array({message_item()})) {
    return {{"id", "resp_1"}, {"object", "response"}, {"status", "completed"},
        {"output", std::move(output)}, {"usage", {{"input_tokens", 8},
            {"output_tokens", 3}, {"total_tokens", 11},
            {"input_tokens_details", {{"cached_tokens", 4}}},
            {"output_tokens_details", {{"reasoning_tokens", 2}}}}}};
}

std::string frame(const Json& event, const std::string& delimiter = "\n\n") {
    return "data: " + event.dump() + delimiter;
}

std::string completed_stream(const std::string& text = "ok") {
    return frame({{"type", "response.completed"},
        {"response", completed(Json::array({message_item(text)}))}});
}

std::vector<StreamEvent> collect(OpenAiResponsesProvider& provider,
    std::atomic<bool>* abort_flag = nullptr) {
    std::vector<StreamEvent> events;
    provider.chat_stream({user_message()}, {lookup_tool()},
        [&](const StreamEvent& event) { events.push_back(event); }, abort_flag);
    return events;
}

TEST(OpenAiResponsesProviderTest, AceModelFactoryDefaultsAndExplicitChatOverride) {
    std::atomic<int> responses_requests{0};
    std::atomic<int> chat_requests{0};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request& request, httplib::Response& response) {
            ++responses_requests;
            const auto body = Json::parse(request.body);
            EXPECT_FALSE(body.contains("messages"));
            EXPECT_TRUE(body.contains("input"));
            EXPECT_EQ(body["store"], false);
            response.set_content(body.value("stream", false) ? completed_stream() : completed().dump(),
                body.value("stream", false) ? "text/event-stream" : "application/json");
        });
        http.Post("/v1/chat/completions", [&](const httplib::Request&, httplib::Response& response) {
            ++chat_requests;
            response.set_content(
                R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})",
                "application/json");
        });
    });
    ModelProfile profile;
    profile.name = "ace-default";
    profile.provider = "openai";
    profile.models_dev_provider_id = "acemodel";
    profile.base_url = server.url();
    profile.api_key = "test-key";
    for (const auto* model : {"moonlight", "starrylight", "aurora"}) {
        profile.model = model;
        auto provider = create_provider_from_entry(profile);
        ASSERT_TRUE(provider);
        EXPECT_EQ(provider->chat({user_message()}, {}).content, "ok");
        EXPECT_EQ(provider->chat_for_compaction({user_message()}, {}, nullptr).content, "ok");
        std::string streamed;
        provider->chat_stream({user_message()}, {}, [&](const StreamEvent& event) {
            if (event.type == StreamEventType::Delta) streamed += event.content;
        });
        EXPECT_EQ(streamed, "ok");
    }
    const auto implicit = prepare_provider_construction(profile);
    ASSERT_TRUE(implicit);
    profile.api_protocol = "responses";
    const auto explicit_responses = prepare_provider_construction(profile);
    ASSERT_TRUE(explicit_responses);
    EXPECT_EQ(implicit->fingerprint(), explicit_responses->fingerprint());
    profile.api_protocol = "chat_completions";
    const auto explicit_chat = prepare_provider_construction(profile);
    ASSERT_TRUE(explicit_chat);
    EXPECT_NE(implicit->fingerprint(), explicit_chat->fingerprint());
    auto provider = explicit_chat->construct().provider;
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->chat({user_message()}, {}).content, "ok");
    profile.api_protocol.reset();
    profile.models_dev_provider_id.reset();
    provider = create_provider_from_entry(profile);
    ASSERT_TRUE(provider);
    EXPECT_EQ(provider->chat({user_message()}, {}).content, "ok");
    EXPECT_EQ(responses_requests.load(), 9);
    EXPECT_EQ(chat_requests.load(), 2);
}

int count_events(const std::vector<StreamEvent>& events, StreamEventType type) {
    return static_cast<int>(std::count_if(events.begin(), events.end(),
        [type](const StreamEvent& event) { return event.type == type; }));
}

const StreamEvent* last_event(const std::vector<StreamEvent>& events, StreamEventType type) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->type == type) return &*it;
    }
    return nullptr;
}

TEST(OpenAiResponsesProviderTest, SendsResponsesShapeHeadersAndConfiguredLimits) {
    Json received;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request& request,
                                      httplib::Response& response) {
            received = Json::parse(request.body);
            EXPECT_EQ(request.get_header_value("Authorization"), "Bearer override");
            EXPECT_EQ(request.get_header_value("X-Project"), "project-1");
            EXPECT_EQ(request.get_header_value("Accept"), "application/json");
            response.set_content(completed().dump(), "application/json");
        });
    });
    ProviderRequestOptions options;
    options.max_output_tokens = 1200;
    options.reasoning_protocol = ReasoningWireProtocol::OpenAi;
    options.reasoning = ModelReasoningOptions{};
    options.reasoning->supported = true;
    options.reasoning->enabled = true;
    options.reasoning->effort = "high";
    options.reasoning->supported_efforts = {"low", "high"};
    OpenAiResponsesProvider provider(server.url() + "/", "original", "test-model",
        5000, {{"Authorization", "Bearer override"}, {"X-Project", "project-1"}}, options);
    const auto response = provider.chat({user_message()}, {lookup_tool()});
    EXPECT_EQ(provider.name(), "openai");
    EXPECT_EQ(response.content, "ok");
    EXPECT_EQ(response.usage.cache_read_tokens, 4);
    EXPECT_EQ(response.usage.reasoning_tokens, 2);
    EXPECT_EQ(response.usage.total_tokens, 11);
    EXPECT_EQ(received["store"], false);
    EXPECT_EQ(received["model"], "test-model");
    EXPECT_EQ(received["max_output_tokens"], 1200);
    EXPECT_EQ(received["reasoning"]["effort"], "high");
    EXPECT_EQ(received["reasoning"]["summary"], "auto");
    EXPECT_EQ(received["tools"][0]["strict"], false);
    EXPECT_EQ(received["tools"][0]["name"], "lookup");
    EXPECT_FALSE(received.contains("messages"));
    EXPECT_FALSE(received.contains("max_tokens"));
    EXPECT_FALSE(received.contains("stream_options"));
    EXPECT_FALSE(received.contains("previous_response_id"));
}

TEST(OpenAiResponsesProviderTest, UsesExactFullUrlAndKeepsMissingReasoningToggleDefault) {
    Json received;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/deployment/model/responses", [&](const httplib::Request& request,
                                                     httplib::Response& response) {
            EXPECT_EQ(request.get_param_value("api-version"), "preview");
            received = Json::parse(request.body);
            response.set_content(completed().dump(), "application/json");
        });
    });
    ProviderRequestOptions options;
    options.endpoint_mode = "full_url";
    options.reasoning = ModelReasoningOptions{};
    options.reasoning->supported = true;
    OpenAiResponsesProvider provider(server.url("/deployment/model/responses?api-version=preview"),
        "", "test-model", 5000, {}, options);
    EXPECT_EQ(provider.chat({user_message()}, {}).content, "ok");
    EXPECT_FALSE(received.contains("reasoning"));
    options.reasoning->enabled = false;
    provider.reconfigure(server.url("/deployment/model/responses?api-version=preview"),
        "", 5000, {}, options);
    EXPECT_EQ(provider.chat({user_message()}, {}).content, "ok");
    EXPECT_EQ(received["reasoning"]["effort"], "none");
}

TEST(OpenAiResponsesProviderTest, ReplaysEncryptedReasoningAndOriginalToolCallOnNextTurn) {
    const Json reasoning{{"id", "rs_1"}, {"type", "reasoning"},
        {"encrypted_content", "opaque-encrypted-state"}, {"summary", Json::array()}};
    std::vector<Json> requests;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request& request,
                                      httplib::Response& response) {
            requests.push_back(Json::parse(request.body));
            response.set_content((requests.size() == 1
                ? completed(Json::array({reasoning, call_item()}))
                : completed()).dump(), "application/json");
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    const auto first = provider.chat({user_message()}, {lookup_tool()});
    ASSERT_EQ(first.tool_calls.size(), 1u);
    EXPECT_EQ(first.tool_calls[0].id, "call_1");
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = first.content;
    assistant.content_parts = first.content_parts;
    assistant.tool_calls = Json::array({{{"id", first.tool_calls[0].id},
        {"type", "function"}, {"function", {{"name", first.tool_calls[0].function_name},
            {"arguments", first.tool_calls[0].function_arguments}}}}});
    ChatMessage result;
    result.role = "tool";
    result.tool_call_id = "call_1";
    result.content = "sunny";
    EXPECT_EQ(provider.chat({user_message(), assistant, result}, {lookup_tool()}).content, "ok");
    ASSERT_EQ(requests.size(), 2u);
    const auto& input = requests[1]["input"];
    ASSERT_EQ(input.size(), 4u);
    EXPECT_EQ(input[1], reasoning);
    EXPECT_EQ(input[2]["type"], "function_call");
    EXPECT_EQ(input[2]["call_id"], "call_1");
    EXPECT_EQ(input[3]["type"], "function_call_output");
    EXPECT_EQ(input[3]["call_id"], "call_1");
    EXPECT_EQ(input[3]["output"], "sunny");
}

TEST(OpenAiResponsesProviderTest, OptionsAndCompactionDispatchStayOnResponses) {
    std::vector<Json> requests;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request& request,
                                      httplib::Response& response) {
            requests.push_back(Json::parse(request.body));
            if (requests.back().value("stream", false)) {
                response.set_content(completed_stream(), "text/event-stream");
            } else {
                response.set_content(completed().dump(), "application/json");
            }
        });
    });
    CacheResponsesProvider provider(server.url(), "", "test-model", 5000);
    LlmProvider& polymorphic = provider;
    polymorphic.set_prompt_cache_key("session-default");
    ChatRequestOptions options;
    options.prompt_cache_key = "request-owned";
    options.for_compaction = true;
    EXPECT_EQ(polymorphic.chat_for_compaction({user_message()}, {lookup_tool()}, nullptr).content, "ok");
    EXPECT_EQ(polymorphic.chat_with_options({user_message()}, {lookup_tool()}, options, nullptr).content, "ok");
    std::vector<StreamEvent> events;
    polymorphic.chat_stream_with_options({user_message()}, {lookup_tool()}, options,
        [&](const StreamEvent& event) { events.push_back(event); });
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_EQ(requests[0]["prompt_cache_key"], "session-default");
    EXPECT_EQ(requests[1]["prompt_cache_key"], "request-owned");
    EXPECT_EQ(requests[2]["prompt_cache_key"], "request-owned");
    for (const auto& request : requests) {
        EXPECT_EQ(request["tool_choice"], "none");
        EXPECT_EQ(request["tools"][0]["name"], "lookup");
    }
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
    EXPECT_FALSE(polymorphic.supports_native_compaction());
}

TEST(OpenAiResponsesProviderTest, PromptCacheFieldRejectionRetriesOnlyOnceAndRemembersScope) {
    std::atomic<int> rejected{0};
    std::atomic<int> accepted{0};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request& request,
                                      httplib::Response& response) {
            const auto body = Json::parse(request.body);
            if (body.contains("prompt_cache_key")) {
                ++rejected;
                response.status = 400;
                response.set_content(R"({"error":{"param":"prompt_cache_key","code":"unsupported_parameter"}})",
                    "application/json");
            } else {
                ++accepted;
                response.set_content(body.value("stream", false) ? completed_stream() : completed().dump(),
                    body.value("stream", false) ? "text/event-stream" : "application/json");
            }
        });
    });
    CacheResponsesProvider provider(server.url(), "", "test-model", 5000);
    provider.set_prompt_cache_key("cache-key");
    const auto events = collect(provider);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
    EXPECT_EQ(provider.chat({user_message()}, {}).content, "ok");
    EXPECT_EQ(rejected.load(), 1);
    EXPECT_EQ(accepted.load(), 2);
}

TEST(OpenAiResponsesProviderTest, ParsesFragmentedCrLfStreamAndFinalToolExactlyOnce) {
    const auto call = call_item();
    const auto sse = std::make_shared<const std::string>(
        ": heartbeat\r\n\r\n" +
        frame({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", call}}, "\r\n\r\n") +
        frame({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", call}}, "\r\n\r\n") +
        frame({{"type", "response.completed"}, {"response", completed(Json::array({call}))}}, "\r\n\r\n") +
        "data: [DONE]\r\n\r\n");
    LocalHttpServer server([sse](httplib::Server& http) {
        http.Post("/v1/responses", [sse](const httplib::Request& request,
                                        httplib::Response& response) {
            EXPECT_EQ(request.get_header_value("Accept"), "text/event-stream");
            EXPECT_EQ(request.get_header_value("Accept-Encoding"), "identity");
            response.set_chunked_content_provider("text/event-stream",
                [sse](std::size_t offset, httplib::DataSink& sink) {
                    const auto length = (std::min)(std::size_t(13), sse->size() - offset);
                    sink.write(sse->data() + offset, length);
                    if (offset + length == sse->size()) sink.done();
                    return true;
                });
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    const auto events = collect(provider);
    EXPECT_EQ(count_events(events, StreamEventType::Error), 0);
    EXPECT_EQ(count_events(events, StreamEventType::ToolCall), 1);
    EXPECT_EQ(count_events(events, StreamEventType::Usage), 1);
    ASSERT_EQ(count_events(events, StreamEventType::Done), 1);
    EXPECT_EQ(last_event(events, StreamEventType::Done)->finish_reason, "tool_calls");
}

TEST(OpenAiResponsesProviderTest, TruncatedStreamResetsProvisionalOutputAndCallsBeforeRetry) {
    std::atomic<int> requests{0};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
            response.set_header("Retry-After", "0");
            if (++requests == 1) {
                response.set_content(
                    frame({{"type", "response.output_text.delta"}, {"item_id", "msg_old"},
                        {"output_index", 0}, {"content_index", 0}, {"delta", "old"}}) +
                    frame({{"type", "response.output_item.done"}, {"output_index", 1},
                        {"item", call_item("call_old")}}) + "data: [DONE]\n\n", "text/event-stream");
            } else {
                response.set_content(completed_stream("new"), "text/event-stream");
            }
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    std::string visible;
    std::vector<StreamEvent> events;
    provider.chat_stream({user_message()}, {lookup_tool()}, [&](const StreamEvent& event) {
        events.push_back(event);
        if (event.type == StreamEventType::Retry) visible.clear();
        if (event.type == StreamEventType::Delta) visible += event.content;
    });
    EXPECT_EQ(requests.load(), 2);
    EXPECT_EQ(visible, "new");
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 1);
    EXPECT_EQ(count_events(events, StreamEventType::RetryResume), 1);
    EXPECT_EQ(count_events(events, StreamEventType::ToolCall), 0);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
    EXPECT_EQ(count_events(events, StreamEventType::Error), 0);
}

TEST(OpenAiResponsesProviderTest, HardQuotaAndMalformedJsonAreTerminalWithDiagnostics) {
    const std::vector<std::pair<int, std::string>> failures{
        {429, R"({"error":{"code":"insufficient_quota","message":"buy credits"}})"},
        {200, "data: {not-json}\n\n"},
        {200, frame({{"type", "response.failed"}, {"response", {
            {"status", "failed"}, {"error", {{"code", "insufficient_quota"},
                {"message", "buy credits"}}}}}})}
    };
    for (const auto& [status, body] : failures) {
        SCOPED_TRACE(body);
        std::atomic<int> requests{0};
        LocalHttpServer server([&](httplib::Server& http) {
            http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
                ++requests;
                response.status = status;
                response.set_header("X-Request-Id", "request-fixture");
                response.set_content(body, status == 200 ? "text/event-stream" : "application/json");
            });
        });
        OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
        const auto events = collect(provider);
        ASSERT_EQ(count_events(events, StreamEventType::Error), 1);
        EXPECT_EQ(count_events(events, StreamEventType::Retry), 0);
        EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
        EXPECT_EQ(requests.load(), 1);
        const auto& error = last_event(events, StreamEventType::Error)->provider_error;
        EXPECT_FALSE(error.retryable);
        EXPECT_EQ(error.request_id, "request-fixture");
        EXPECT_EQ(error.provider, "openai");
        EXPECT_EQ(error.model, "test-model");
        EXPECT_FALSE(error.raw_body.empty());
    }
}

TEST(OpenAiResponsesProviderTest, TransportFailureAfterTerminalStillSuppressesToolsUntilRetry) {
    std::atomic<int> requests{0};
    // Shared between the handler and the response content callback.
    const auto payload = std::make_shared<const std::string>(frame({
        {"type", "response.completed"}, {"response", completed(Json::array({call_item()}))}}));
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
            response.set_header("Retry-After", "0");
            if (++requests == 1) {
                // A complete terminal frame followed by premature HTTP EOF.
                response.set_content_provider(payload->size() + 16, "text/event-stream",
                    [payload](std::size_t offset, std::size_t, httplib::DataSink& sink) {
                        if (offset >= payload->size()) return false;
                        sink.write(payload->data() + offset, payload->size() - offset);
                        return false;
                    });
            } else {
                response.set_content(completed_stream("recovered"), "text/event-stream");
            }
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    const auto events = collect(provider);
    EXPECT_EQ(requests.load(), 2);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 1);
    EXPECT_EQ(count_events(events, StreamEventType::ToolCall), 0);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
    EXPECT_EQ(count_events(events, StreamEventType::Error), 0);
}

TEST(OpenAiResponsesProviderTest, HttpRateLimitAndSseServerFailureRetryWithSamePolicy) {
    for (const bool sse_failure : {false, true}) {
        std::atomic<int> requests{0};
        LocalHttpServer server([&](httplib::Server& http) {
            http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
                response.set_header("Retry-After", "0");
                if (++requests == 1) {
                    response.status = sse_failure ? 200 : 429;
                    response.set_content(sse_failure
                        ? frame({{"type", "response.failed"}, {"response", {{"status", "failed"},
                            {"error", {{"code", "server_error"}, {"message", "try again"}}}}}})
                        : R"({"error":{"code":"rate_limit_exceeded","message":"try again"}})",
                        sse_failure ? "text/event-stream" : "application/json");
                } else {
                    response.set_content(completed_stream(), "text/event-stream");
                }
            });
        });
        OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
        const auto events = collect(provider);
        EXPECT_EQ(requests.load(), 2);
        EXPECT_EQ(count_events(events, StreamEventType::Retry), 1);
        EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
        EXPECT_EQ(count_events(events, StreamEventType::Error), 0);
    }
}

TEST(OpenAiResponsesProviderTest, PreCancelledCallsAndUnresolvedHeadersNeverSendRequest) {
    std::atomic<int> requests{0};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
            ++requests;
            response.set_content(completed().dump(), "application/json");
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    std::atomic<bool> abort{true};
    EXPECT_EQ(provider.chat_cancellable({user_message()}, {}, &abort).provider_error.kind,
        ProviderErrorKind::UserCancelled);
    EXPECT_EQ(provider.chat_for_compaction({user_message()}, {}, &abort).provider_error.kind,
        ProviderErrorKind::UserCancelled);
    EXPECT_EQ(provider.chat_with_options({user_message()}, {}, {}, &abort).provider_error.kind,
        ProviderErrorKind::UserCancelled);
    const auto cancelled = collect(provider, &abort);
    ASSERT_EQ(cancelled.size(), 1u);
    EXPECT_EQ(cancelled[0].provider_error.kind, ProviderErrorKind::UserCancelled);
    OpenAiResponsesProvider bad_headers(server.url(), "", "test-model", 5000,
        {{"X-Project", "${env:ACECODE_RESPONSES_FIXTURE_MISSING_ENV_VARIABLE}"}});
    EXPECT_TRUE(bad_headers.chat({user_message()}, {}).provider_error.has_error());
    EXPECT_EQ(count_events(collect(bad_headers), StreamEventType::Error), 1);
    EXPECT_EQ(requests.load(), 0);
}

TEST(OpenAiResponsesProviderTest, CancellingPartialStreamSuppressesTerminalTools) {
    LocalHttpServer server([](httplib::Server& http) {
        http.Post("/v1/responses", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(frame({{"type", "response.output_text.delta"},
                {"item_id", "msg_1"}, {"output_index", 0}, {"content_index", 0}, {"delta", "hello"}}) +
                frame({{"type", "response.completed"}, {"response", completed(Json::array({call_item()}))}}),
                "text/event-stream");
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    std::atomic<bool> abort{false};
    std::vector<StreamEvent> events;
    provider.chat_stream({user_message()}, {lookup_tool()}, [&](const StreamEvent& event) {
        events.push_back(event);
        if (event.type == StreamEventType::Delta) abort.store(true);
    }, &abort);
    EXPECT_EQ(count_events(events, StreamEventType::Delta), 1);
    EXPECT_EQ(count_events(events, StreamEventType::ToolCall), 0);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
    ASSERT_EQ(count_events(events, StreamEventType::Error), 1);
    EXPECT_EQ(last_event(events, StreamEventType::Error)->provider_error.kind,
        ProviderErrorKind::UserCancelled);
}

TEST(OpenAiResponsesProviderTest, NonStreamingCancellationAndFailureKeepTypedMetadata) {
    std::atomic<bool> abort{false};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
            abort.store(true);
            response.status = 429;
            response.set_header("Retry-After", "2");
            response.set_header("X-Request-Id", "non-stream-request");
            response.set_content(R"({"error":{"code":"rate_limit_exceeded"}})", "application/json");
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    const auto cancelled = provider.chat_cancellable({user_message()}, {}, &abort);
    EXPECT_EQ(cancelled.provider_error.kind, ProviderErrorKind::UserCancelled);
    const auto response = provider.chat({user_message()}, {});
    EXPECT_EQ(response.finish_reason, "error");
    EXPECT_TRUE(response.provider_error.retryable);
    EXPECT_EQ(response.provider_error.server_retry_after_ms, 2000);
    EXPECT_EQ(response.provider_error.request_id, "non-stream-request");
}

TEST(OpenAiResponsesProviderTest, CancellationDuringRetryWaitStopsFurtherAttempts) {
    std::atomic<int> requests{0};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
            ++requests;
            response.status = 503;
            response.set_header("Retry-After", "120");
            response.set_content("unavailable", "text/plain");
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
    std::atomic<bool> abort{false};
    std::vector<StreamEvent> events;
    provider.chat_stream({user_message()}, {}, [&](const StreamEvent& event) {
        events.push_back(event);
        if (event.type == StreamEventType::Retry) abort.store(true);
    }, &abort);
    EXPECT_EQ(requests.load(), 1);
    EXPECT_EQ(count_events(events, StreamEventType::RetryResume), 0);
    ASSERT_EQ(count_events(events, StreamEventType::Error), 1);
    EXPECT_EQ(last_event(events, StreamEventType::Error)->provider_error.kind,
        ProviderErrorKind::UserCancelled);
}

TEST(OpenAiResponsesProviderTest, GatewayErrorMeaningOverridesStatusForRetries) {
    struct Case {
        int status;
        std::string body;
        bool retryable;
    };
    const std::vector<Case> cases{
        {400, R"json({"message":"您的请求频率已达到限制 (100次/10分钟)"})json", true},
        {429, R"({"message":"请求上下文过大"})", false},
        {429, R"({"error":{"code":"insufficient_quota"}})", false},
    };
    for (const auto& sample : cases) {
        SCOPED_TRACE(sample.body);
        std::atomic<int> requests{0};
        LocalHttpServer server([&](httplib::Server& http) {
            http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
                ++requests;
                response.status = sample.status;
                response.set_content(sample.body, "application/json");
            });
        });
        OpenAiResponsesProvider provider(server.url(), "", "test-model", 5000);
        std::atomic<bool> abort{false};
        std::vector<StreamEvent> events;
        provider.chat_stream({user_message()}, {}, [&](const StreamEvent& event) {
            events.push_back(event);
            if (event.type == StreamEventType::Retry) abort.store(true);
        }, &abort);
        EXPECT_EQ(requests.load(), 1);
        EXPECT_EQ(count_events(events, StreamEventType::Retry), sample.retryable ? 1 : 0);
        EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
        ASSERT_EQ(count_events(events, StreamEventType::Error), 1);
        EXPECT_EQ(last_event(events, StreamEventType::Error)->provider_error.kind,
            sample.retryable ? ProviderErrorKind::UserCancelled : ProviderErrorKind::Http);
    }
}

TEST(OpenAiResponsesProviderTest, IdleTimeoutBeforeFirstBytesIsCancellableAndRetryable) {
    std::atomic<int> requests{0};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/v1/responses", [&](const httplib::Request&, httplib::Response& response) {
            ++requests;
            std::this_thread::sleep_for(1200ms);
            response.set_content(completed_stream(), "text/event-stream");
        });
    });
    OpenAiResponsesProvider provider(server.url(), "", "test-model", 100);
    std::atomic<bool> abort{false};
    std::vector<StreamEvent> events;
    provider.chat_stream({user_message()}, {}, [&](const StreamEvent& event) {
        events.push_back(event);
        if (event.type == StreamEventType::Retry) abort.store(true);
    }, &abort);
    EXPECT_EQ(requests.load(), 1);
    ASSERT_EQ(count_events(events, StreamEventType::Retry), 1);
    EXPECT_EQ(last_event(events, StreamEventType::Retry)->provider_error.kind,
        ProviderErrorKind::Timeout);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
    ASSERT_EQ(count_events(events, StreamEventType::Error), 1);
    EXPECT_EQ(last_event(events, StreamEventType::Error)->provider_error.kind,
        ProviderErrorKind::UserCancelled);
}

} // namespace
